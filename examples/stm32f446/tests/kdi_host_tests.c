#include <stdint.h>
#include <stdio.h>

#include "kdi.h"

static int failures = 0;
static KdiCapToken token_kernel = KDI_CAP_INVALID;
static KdiCapToken token_uart = KDI_CAP_INVALID;
static KdiCapToken token_sensor = KDI_CAP_INVALID;
static KdiCapToken token_vm = KDI_CAP_INVALID;
static KdiCapToken token_diag = KDI_CAP_INVALID;
static uint32_t irq_worker_calls = 0U;
static uint32_t irq_worker_arg_sum = 0U;
static uint16_t irq_worker_last_id = 0U;

static void check(int cond, const char *msg)
{
  if (!cond) {
    printf("FAIL: %s\n", msg);
    failures++;
  }
}

static void check_state(KdiDriverId driver, KdiDriverState expected, const char *msg)
{
  KdiDriverState state = KDI_STATE_DEAD;
  int rc = kdi_driver_get_state(driver, &state);
  check(rc == KDI_OK, "driver state query");
  check(rc == KDI_OK && state == expected, msg);
}

static void boot_driver_to_active(KdiDriverId driver, KdiCapToken token, const char *label)
{
  char msg[96];

  snprintf(msg, sizeof(msg), "%s probe", label);
  check(kdi_driver_probe(driver, token) == KDI_OK, msg);
  snprintf(msg, sizeof(msg), "%s ready", label);
  check(kdi_driver_probe_done(driver, token, 1U) == KDI_OK, msg);
  snprintf(msg, sizeof(msg), "%s activate", label);
  check(kdi_driver_activate(driver, token) == KDI_OK, msg);
  check_state(driver, KDI_STATE_ACTIVE, "driver booted to active");
}

static int test_irq_worker(KdiDriverId driver, uint16_t work_id, uint32_t arg, void *ctx)
{
  uint32_t *sum = (uint32_t *)ctx;

  if (driver != KDI_DRIVER_UART) {
    return KDI_ERR_BAD_ARG;
  }
  irq_worker_calls++;
  irq_worker_last_id = work_id;
  irq_worker_arg_sum += arg;
  if (sum != NULL) {
    *sum += arg;
  }
  if (work_id == 0xEEU) {
    return KDI_ERR_UNSUPPORTED;
  }
  return KDI_OK;
}

static void test_policy_matrix(void)
{
  const KdiPolicy *kernel = kdi_get_policy(KDI_DRIVER_KERNEL);
  const KdiPolicy *sensor = kdi_get_policy(KDI_DRIVER_SENSOR);

  check(kernel != NULL, "kernel policy exists");
  check(sensor != NULL, "sensor policy exists");
  check(kernel != NULL && kernel->allow_reset == 1U, "kernel may request reset");
  check(sensor != NULL && sensor->allow_reset == 0U, "sensor reset denied by policy");
}

static void test_mpu_contract(void)
{
  KdiMpuRequest ok = {
    .region_index = 1U,
    .base = 0x20001000U,
    .size = 256U,
    .attrs = 0U,
  };
  KdiMpuRequest bad_align = ok;

  bad_align.base = 0x20001010U;

  check(kdi_request_mpu_region(KDI_DRIVER_VM_RUNTIME, token_vm, &ok) == KDI_OK, "vm runtime mpu allow");
  check(kdi_request_mpu_region(KDI_DRIVER_VM_RUNTIME, token_vm, &bad_align) == KDI_ERR_BAD_ARG, "mpu base alignment check");
  check(kdi_request_mpu_region(KDI_DRIVER_SENSOR, token_sensor, &ok) == KDI_ERR_DENIED, "sensor mpu denied");
}

static void test_irq_and_dma_contract(void)
{
  KdiIrqRequest irq_ok = {.irqn = 38, .priority = 5};
  KdiIrqRequest irq_bad = {.irqn = 120, .priority = 2};
  KdiDmaRequest dma_ok = {.base = 0x20000200U, .size = 512U, .align = 16U, .direction = 1U};
  KdiDmaRequest dma_big = {.base = 0x20000200U, .size = 4096U, .align = 16U, .direction = 1U};

  check(kdi_request_irq(KDI_DRIVER_UART, token_uart, &irq_ok) == KDI_OK, "uart irq allow");
  check(kdi_request_irq(KDI_DRIVER_UART, token_uart, &irq_bad) == KDI_ERR_LIMIT, "uart irq range reject");
  check(kdi_request_irq(KDI_DRIVER_VM_RUNTIME, token_vm, &irq_ok) == KDI_ERR_DENIED, "vm irq denied");

  check(kdi_declare_dma_buffer(KDI_DRIVER_SENSOR, token_sensor, &dma_ok) == KDI_OK, "sensor dma allow");
  check(kdi_declare_dma_buffer(KDI_DRIVER_SENSOR, token_sensor, &dma_big) == KDI_ERR_LIMIT, "sensor dma limit reject");
  check(kdi_declare_dma_buffer(KDI_DRIVER_DIAG, token_diag, &dma_ok) == KDI_ERR_DENIED, "diag dma denied");
}

static void test_dma_zero_copy_lifecycle(void)
{
  KdiDmaRingConfig cfg = {.rx_depth = 2U, .tx_depth = 2U};
  KdiDmaRequest b0 = {.base = 0x20002000U, .size = 512U, .align = 16U, .direction = 1U};
  KdiDmaRequest b1 = {.base = 0x20002200U, .size = 512U, .align = 16U, .direction = 1U};
  KdiDmaRequest b2 = {.base = 0x20002400U, .size = 512U, .align = 16U, .direction = 1U};
  KdiDmaOwner owner = KDI_DMA_OWNER_DRIVER;
  KdiDmaLeakReport leak = {0};
  KdiDmaStats dma_stats = {0};
  KdiDmaRingOccupancy occ = {0};
  uint16_t rx0 = 0U;
  uint16_t rx1 = 0U;
  uint16_t tx0 = 0U;
  uint16_t hw_id = 0U;
  uint16_t poll_id = 0U;
  uint16_t poll_bytes = 0U;

  check(kdi_dma_ring_configure(KDI_DRIVER_SENSOR, token_sensor, &cfg) == KDI_OK, "configure dma rings");
  check(kdi_dma_register_buffer(KDI_DRIVER_SENSOR, token_sensor, &b0, &rx0) == KDI_OK, "register dma buffer rx0");
  check(kdi_dma_register_buffer(KDI_DRIVER_SENSOR, token_sensor, &b1, &rx1) == KDI_OK, "register dma buffer rx1");
  check(kdi_dma_register_buffer(KDI_DRIVER_SENSOR, token_sensor, &b2, &tx0) == KDI_OK, "register dma buffer tx0");
  check(kdi_dma_register_buffer(KDI_DRIVER_SENSOR, token_sensor, &b2, &tx0) == KDI_ERR_STATE,
        "duplicate dma base rejected");

  check(kdi_dma_get_owner(KDI_DRIVER_SENSOR, token_sensor, rx0, &owner) == KDI_OK, "query owner after register");
  check(owner == KDI_DMA_OWNER_DRIVER, "new buffer starts driver-owned");

  check(kdi_dma_rx_post_buffer(KDI_DRIVER_SENSOR, token_sensor, rx0) == KDI_OK, "post rx0 to rx ring");
  check(kdi_dma_rx_post_buffer(KDI_DRIVER_SENSOR, token_sensor, rx1) == KDI_OK, "post rx1 to rx ring");
  check(kdi_dma_rx_post_buffer(KDI_DRIVER_SENSOR, token_sensor, tx0) == KDI_ERR_LIMIT, "rx ring depth limit");
  check(kdi_dma_get_ring_occupancy(KDI_DRIVER_SENSOR, &occ) == KDI_OK, "dma occupancy after rx post");
  check(occ.rx_posted == 2U && occ.rx_ready == 0U, "dma occupancy tracks rx posted");
  check(occ.tx_pending == 0U && occ.tx_done == 0U, "dma occupancy tracks tx empty");
  check(occ.rx_depth == 2U && occ.tx_depth == 2U, "dma occupancy keeps configured depth");
  check(occ.total_buffers == 3U, "dma occupancy tracks registered buffers");
  check(kdi_dma_get_owner(KDI_DRIVER_SENSOR, token_sensor, rx0, &owner) == KDI_OK, "query owner after rx post");
  check(owner == KDI_DMA_OWNER_KERNEL, "posted rx buffer is kernel-owned");

  check(kdi_dma_rx_complete_one(KDI_DRIVER_SENSOR, token_kernel, 128U, &hw_id) == KDI_OK, "kernel completes rx0");
  check(hw_id == rx0, "rx completion fifo first");
  check(kdi_dma_rx_complete_one(KDI_DRIVER_SENSOR, token_kernel, 96U, &hw_id) == KDI_OK, "kernel completes rx1");
  check(hw_id == rx1, "rx completion fifo second");
  check(kdi_dma_rx_complete_one(KDI_DRIVER_SENSOR, token_kernel, 64U, &hw_id) == KDI_ERR_STATE,
        "rx completion requires posted buffers");

  check(kdi_dma_rx_poll(KDI_DRIVER_SENSOR, token_sensor, &poll_id, &poll_bytes) == KDI_OK, "driver polls rx0");
  check(poll_id == rx0 && poll_bytes == 128U, "rx poll returns bytes for rx0");
  check(kdi_dma_get_owner(KDI_DRIVER_SENSOR, token_sensor, rx0, &owner) == KDI_OK, "owner after rx poll");
  check(owner == KDI_DMA_OWNER_DRIVER, "rx poll hands ownership back to driver");
  check(kdi_dma_rx_poll(KDI_DRIVER_SENSOR, token_sensor, &poll_id, &poll_bytes) == KDI_OK, "driver polls rx1");
  check(poll_id == rx1 && poll_bytes == 96U, "rx poll returns bytes for rx1");
  check(kdi_dma_rx_poll(KDI_DRIVER_SENSOR, token_sensor, &poll_id, &poll_bytes) == KDI_ERR_STATE, "rx poll empty");

  check(kdi_dma_tx_submit(KDI_DRIVER_SENSOR, token_sensor, tx0, 200U) == KDI_OK, "submit tx0");
  check(kdi_dma_tx_submit(KDI_DRIVER_SENSOR, token_sensor, rx0, 220U) == KDI_OK, "submit rx0 as tx");
  check(kdi_dma_tx_submit(KDI_DRIVER_SENSOR, token_sensor, rx1, 240U) == KDI_ERR_LIMIT, "tx ring depth limit");
  check(kdi_dma_get_ring_occupancy(KDI_DRIVER_SENSOR, &occ) == KDI_OK, "dma occupancy after tx submit");
  check(occ.tx_pending == 2U && occ.tx_done == 0U, "dma occupancy tracks tx pending");
  check(kdi_dma_get_owner(KDI_DRIVER_SENSOR, token_sensor, tx0, &owner) == KDI_OK, "query tx owner");
  check(owner == KDI_DMA_OWNER_KERNEL, "submitted tx buffer kernel-owned");

  check(kdi_dma_tx_complete_one(KDI_DRIVER_SENSOR, token_kernel, &hw_id) == KDI_OK, "kernel completes tx0");
  check(hw_id == tx0, "tx completion fifo first");
  check(kdi_dma_tx_complete_one(KDI_DRIVER_SENSOR, token_kernel, &hw_id) == KDI_OK, "kernel completes rx0");
  check(hw_id == rx0, "tx completion fifo second");
  check(kdi_dma_tx_complete_one(KDI_DRIVER_SENSOR, token_kernel, &hw_id) == KDI_ERR_STATE,
        "tx completion requires pending descriptors");

  check(kdi_dma_tx_poll_complete(KDI_DRIVER_SENSOR, token_sensor, &poll_id) == KDI_OK, "driver polls tx0 done");
  check(poll_id == tx0, "tx poll fifo first");
  check(kdi_dma_tx_poll_complete(KDI_DRIVER_SENSOR, token_sensor, &poll_id) == KDI_OK, "driver polls rx0 done");
  check(poll_id == rx0, "tx poll fifo second");
  check(kdi_dma_tx_poll_complete(KDI_DRIVER_SENSOR, token_sensor, &poll_id) == KDI_ERR_STATE, "tx poll empty");

  check(kdi_dma_check_leaks(KDI_DRIVER_SENSOR, token_sensor, &leak) == KDI_OK, "leak check clean path");
  check(leak.leak == 0U, "no dma leak when all buffers reclaimed");
  check(leak.kernel_owned == 0U, "no kernel-owned dma buffers in clean path");

  check(kdi_dma_rx_post_buffer(KDI_DRIVER_SENSOR, token_sensor, rx0) == KDI_OK, "post rx0 for leak probe");
  check(kdi_dma_check_leaks(KDI_DRIVER_SENSOR, token_sensor, &leak) == KDI_OK, "leak check dirty path");
  check(leak.leak == 1U, "leak flagged with posted rx buffer");
  check(leak.rx_posted >= 1U, "leak report sees posted rx");
  check(leak.kernel_owned >= 1U, "leak report sees kernel ownership");
  check(kdi_dma_check_leaks(KDI_DRIVER_SENSOR, token_kernel, &leak) == KDI_OK, "kernel can inspect dma leaks");
  check(kdi_dma_ring_configure(KDI_DRIVER_SENSOR, token_sensor, &cfg) == KDI_ERR_STATE,
        "ring reconfigure blocked with outstanding buffers");

  check(kdi_dma_rx_complete_one(KDI_DRIVER_SENSOR, token_kernel, 64U, &hw_id) == KDI_OK, "complete leak probe rx");
  check(kdi_dma_rx_poll(KDI_DRIVER_SENSOR, token_sensor, &poll_id, &poll_bytes) == KDI_OK, "drain leak probe rx");
  check(kdi_dma_check_leaks(KDI_DRIVER_SENSOR, token_sensor, &leak) == KDI_OK, "leak check clean after drain");
  check(leak.leak == 0U, "leak clears after drain");

  check(kdi_dma_get_stats(KDI_DRIVER_SENSOR, &dma_stats) == KDI_OK, "dma stats query");
  check(dma_stats.rx_post_total >= 3U, "rx post stats count");
  check(dma_stats.rx_complete_total >= 3U, "rx completion stats count");
  check(dma_stats.rx_poll_total >= 3U, "rx poll stats count");
  check(dma_stats.tx_submit_total >= 2U, "tx submit stats count");
  check(dma_stats.tx_complete_total >= 2U, "tx complete stats count");
  check(dma_stats.tx_poll_total >= 2U, "tx poll stats count");
  check(dma_stats.cache_clean_total >= 2U, "cache clean stats count");
  check(dma_stats.cache_invalidate_total >= 3U, "cache invalidate stats count");
  check(dma_stats.ring_overflow_total >= 2U, "ring overflow stats count");
  check(dma_stats.leak_check_total >= 4U, "leak check stats count");
  check(dma_stats.leak_found_total >= 1U, "leak found stats count");

  check(kdi_dma_unregister_buffer(KDI_DRIVER_SENSOR, token_sensor, rx0) == KDI_OK, "unregister rx0");
  check(kdi_dma_unregister_buffer(KDI_DRIVER_SENSOR, token_sensor, rx1) == KDI_OK, "unregister rx1");
  check(kdi_dma_unregister_buffer(KDI_DRIVER_SENSOR, token_sensor, tx0) == KDI_OK, "unregister tx0");
  check(kdi_dma_check_leaks(KDI_DRIVER_SENSOR, token_sensor, &leak) == KDI_OK, "leak check empty table");
  check(leak.total_buffers == 0U, "all dma buffers released");
  check(kdi_dma_get_ring_occupancy(KDI_DRIVER_SENSOR, &occ) == KDI_OK, "dma occupancy after release");
  check(occ.total_buffers == 0U, "dma occupancy sees zero registered buffers");
}

static void test_fault_hook_and_stats(void)
{
  KdiFaultReport report = {.code = 0x12U, .detail = 0xABCDU};
  KdiDriverId driver = KDI_DRIVER_KERNEL;
  KdiFaultReport last = {0};
  KdiStats stats = {0};
  KdiFaultDomainStats domain = {0};

  check(kdi_report_fault(KDI_DRIVER_SENSOR, token_sensor, &report) == KDI_OK, "sensor fault report allow");
  check(kdi_last_fault(&driver, &last) == KDI_OK, "fault report remembered");
  check(driver == KDI_DRIVER_SENSOR, "fault report driver stored");
  check(last.code == report.code, "fault code stored");
  check(last.detail == report.detail, "fault detail stored");

  kdi_get_stats(&stats);
  check(stats.fault_reports >= 1U, "fault stats count");
  check(stats.allow_total >= 1U, "allow stats count");
  check(kdi_fault_domain_get(KDI_DRIVER_SENSOR, &domain) == KDI_OK, "fault domain query");
  check(domain.active_fault == 1U, "fault domain marks active fault");
  check(domain.last_code == report.code, "fault domain stores last code");
  check(domain.last_detail == report.detail, "fault domain stores last detail");
  check(domain.fault_total >= 1U, "fault domain fault count");
}

static void test_fault_domain_containment(void)
{
  KdiDmaRingConfig cfg = {.rx_depth = 2U, .tx_depth = 2U};
  KdiDmaRequest dma = {.base = 0x20003000U, .size = 256U, .align = 16U, .direction = 0U};
  KdiFaultDomainStats uart_before = {0};
  KdiFaultDomainStats uart_after = {0};
  KdiFaultDomainStats sensor_before = {0};
  KdiFaultDomainStats sensor_after = {0};
  KdiIrqDriverStats irq_uart = {0};
  KdiDmaLeakReport leak = {0};
  KdiCapToken rotated = KDI_CAP_INVALID;
  uint16_t dma_id = 0U;
  uint32_t worker_sum = 0U;

  check(kdi_fault_domain_get(KDI_DRIVER_UART, &uart_before) == KDI_OK, "uart fault domain baseline");
  check(kdi_fault_domain_get(KDI_DRIVER_SENSOR, &sensor_before) == KDI_OK, "sensor fault domain baseline");

  check(kdi_dma_ring_configure(KDI_DRIVER_UART, token_uart, &cfg) == KDI_OK, "uart dma ring configure");
  check(kdi_dma_register_buffer(KDI_DRIVER_UART, token_uart, &dma, &dma_id) == KDI_OK, "uart dma register");
  check(kdi_dma_rx_post_buffer(KDI_DRIVER_UART, token_uart, dma_id) == KDI_OK, "uart dma rx post");

  check(kdi_irq_set_worker(KDI_DRIVER_UART, token_uart, test_irq_worker, &worker_sum) == KDI_OK, "uart worker set");
  check(kdi_irq_enter(KDI_DRIVER_UART, token_uart) == KDI_OK, "uart irq enter before crash");
  check(kdi_irq_defer(KDI_DRIVER_UART, token_uart, 0x31U, 1U) == KDI_OK, "uart irq defer before crash");
  check(kdi_irq_exit(KDI_DRIVER_UART, token_uart) == KDI_OK, "uart irq exit before crash");
  check(kdi_irq_get_driver_stats(KDI_DRIVER_UART, &irq_uart) == KDI_OK, "uart irq stats before crash");
  check(irq_uart.deferred_pending >= 1U, "uart has deferred work before crash");

  check(kdi_driver_runtime_error(KDI_DRIVER_UART, token_uart, 0x77U) == KDI_OK, "uart runtime crash");
  check_state(KDI_DRIVER_UART, KDI_STATE_ERROR, "uart isolated in error");
  check_state(KDI_DRIVER_SENSOR, KDI_STATE_ACTIVE, "sensor remains active");
  check_state(KDI_DRIVER_KERNEL, KDI_STATE_ACTIVE, "kernel remains active");

  check(kdi_fault_domain_get(KDI_DRIVER_UART, &uart_after) == KDI_OK, "uart fault domain after crash");
  check(uart_after.active_fault == 1U, "uart fault domain active fault after crash");
  check(uart_after.isolated == 1U, "uart fault domain isolated after crash");
  check(uart_after.last_code == 0x77U, "uart fault domain crash code");
  check(uart_after.fault_total >= uart_before.fault_total + 1U, "uart fault count increments on crash");
  check(uart_after.crash_total >= uart_before.crash_total + 1U, "uart crash count increments");
  check(uart_after.contain_total >= uart_before.contain_total + 1U, "uart contain count increments");

  check(kdi_irq_get_driver_stats(KDI_DRIVER_UART, &irq_uart) == KDI_OK, "uart irq stats after crash");
  check(irq_uart.in_irq == 0U, "uart irq handler cleared by containment");
  check(irq_uart.worker_registered == 0U, "uart worker cleared by containment");
  check(irq_uart.deferred_pending == 0U, "uart deferred queue cleared by containment");

  check(kdi_dma_check_leaks(KDI_DRIVER_UART, token_kernel, &leak) == KDI_OK, "uart dma leak check after crash");
  check(leak.total_buffers == 0U, "uart dma resources reclaimed by containment");

  check(kdi_driver_reset(KDI_DRIVER_UART, token_kernel) == KDI_OK, "uart reset after crash");
  check(kdi_driver_reinit(KDI_DRIVER_UART, token_kernel, &rotated) == KDI_OK, "uart reinit after crash");
  token_uart = rotated;
  check(kdi_driver_probe(KDI_DRIVER_UART, token_uart) == KDI_OK, "uart probe after reinit");
  check(kdi_driver_probe_done(KDI_DRIVER_UART, token_uart, 1U) == KDI_OK, "uart ready after reinit");
  check(kdi_driver_activate(KDI_DRIVER_UART, token_uart) == KDI_OK, "uart active after reinit");
  check_state(KDI_DRIVER_UART, KDI_STATE_ACTIVE, "uart recovers without reboot");
  check_state(KDI_DRIVER_SENSOR, KDI_STATE_ACTIVE, "sensor still active after uart restart");

  check(kdi_fault_domain_get(KDI_DRIVER_UART, &uart_after) == KDI_OK, "uart fault domain after restart");
  check(uart_after.active_fault == 0U, "uart active fault cleared after restart");
  check(uart_after.isolated == 0U, "uart isolation cleared after restart");
  check(uart_after.restart_total >= uart_before.restart_total + 1U, "uart restart count increments");
  check(uart_after.generation >= uart_before.generation + 1U, "uart generation increments on restart");

  check(kdi_fault_domain_get(KDI_DRIVER_SENSOR, &sensor_after) == KDI_OK, "sensor fault domain after uart restart");
  check(sensor_after.generation == sensor_before.generation, "sensor generation unchanged");
  check(sensor_after.crash_total == sensor_before.crash_total, "sensor crash count unchanged");
}

static void test_last_decision(void)
{
  KdiDecision d = {0};
  int rc = kdi_reset_hook(KDI_DRIVER_SENSOR, token_sensor, KDI_RESET_ASSERT);

  check(rc == KDI_ERR_DENIED, "sensor reset denied");
  check(kdi_last_decision(&d) == KDI_OK, "last decision available");
  check(d.req == KDI_REQ_RESET, "last decision request type");
  check(d.driver == KDI_DRIVER_SENSOR, "last decision driver");
  check(d.rc == KDI_ERR_DENIED, "last decision status");
}

static void test_token_contract(void)
{
  KdiIrqRequest irq_ok = {.irqn = 38, .priority = 5};
  KdiStats stats = {0};
  int rc;

  rc = kdi_request_irq(KDI_DRIVER_UART, KDI_CAP_INVALID, &irq_ok);
  check(rc == KDI_ERR_AUTH, "missing token rejected");

  rc = kdi_request_irq(KDI_DRIVER_UART, token_sensor, &irq_ok);
  check(rc == KDI_ERR_AUTH, "wrong token rejected");

  kdi_get_stats(&stats);
  check(stats.auth_fail_total >= 2U, "auth failure stats count");
}

static void test_token_lifecycle(void)
{
  KdiIrqRequest irq_ok = {.irqn = 40, .priority = 4};
  KdiCapToken rotated = KDI_CAP_INVALID;
  KdiStats stats = {0};
  uint8_t active = 1U;

  check(kdi_revoke_token(KDI_DRIVER_UART) == KDI_OK, "revoke uart token");
  check(kdi_token_is_active(KDI_DRIVER_UART, &active) == KDI_OK, "query uart token state");
  check(active == 0U, "uart token inactive after revoke");
  check(kdi_acquire_token(KDI_DRIVER_UART, &rotated) == KDI_ERR_DENIED, "acquire denied after revoke");
  check(kdi_request_irq(KDI_DRIVER_UART, token_uart, &irq_ok) == KDI_ERR_AUTH, "revoked token rejected");

  check(kdi_rotate_token(KDI_DRIVER_UART, &rotated) == KDI_OK, "rotate uart token");
  check(rotated != KDI_CAP_INVALID, "rotated token valid");
  check(rotated != token_uart, "rotated token differs");
  check(kdi_token_is_active(KDI_DRIVER_UART, &active) == KDI_OK, "query uart token state after rotate");
  check(active == 1U, "uart token active after rotate");
  check(kdi_request_irq(KDI_DRIVER_UART, token_uart, &irq_ok) == KDI_ERR_AUTH, "old token invalid after rotate");
  check(kdi_request_irq(KDI_DRIVER_UART, rotated, &irq_ok) == KDI_OK, "new rotated token accepted");

  kdi_get_stats(&stats);
  check(stats.token_revoke_total >= 1U, "token revoke stats count");
  check(stats.token_rotate_total >= 1U, "token rotate stats count");
  token_uart = rotated;
}

static void test_token_ttl_expiry(void)
{
  KdiIrqRequest irq_ok = {.irqn = 36, .priority = 3};
  KdiCapToken sensor_ttl = KDI_CAP_INVALID;
  KdiStats stats = {0};
  uint8_t active = 1U;
  uint32_t rem = 0U;

  kdi_set_now_ms(0U);
  check(kdi_set_token_ttl_ms(KDI_DRIVER_SENSOR, 50U) == KDI_OK, "set sensor ttl");
  check(kdi_rotate_token(KDI_DRIVER_SENSOR, &sensor_ttl) == KDI_OK, "rotate sensor token for ttl test");
  check(kdi_request_irq(KDI_DRIVER_SENSOR, sensor_ttl, &irq_ok) == KDI_OK, "sensor irq ok before ttl expiry");

  kdi_set_now_ms(40U);
  check(kdi_request_irq(KDI_DRIVER_SENSOR, sensor_ttl, &irq_ok) == KDI_OK, "sensor irq ok before ttl deadline");
  check(kdi_token_remaining_ms(KDI_DRIVER_SENSOR, &rem) == KDI_OK, "token remaining query before expiry");
  check(rem <= 10U, "remaining ttl window shrinks");

  kdi_set_now_ms(51U);
  check(kdi_request_irq(KDI_DRIVER_SENSOR, sensor_ttl, &irq_ok) == KDI_ERR_AUTH, "sensor token expires by ttl");
  check(kdi_token_is_active(KDI_DRIVER_SENSOR, &active) == KDI_OK, "token active query after expiry");
  check(active == 0U, "sensor token inactive after ttl expiry");
  check(kdi_token_remaining_ms(KDI_DRIVER_SENSOR, &rem) == KDI_ERR_DENIED, "remaining query denied after expiry");

  kdi_get_stats(&stats);
  check(stats.token_expire_total >= 1U, "token expiry stats count");

  check(kdi_set_token_ttl_ms(KDI_DRIVER_SENSOR, 0U) == KDI_OK, "set sensor ttl to never");
  check(kdi_rotate_token(KDI_DRIVER_SENSOR, &sensor_ttl) == KDI_OK, "rotate sensor token for never ttl");
  kdi_set_now_ms(200000U);
  check(kdi_request_irq(KDI_DRIVER_SENSOR, sensor_ttl, &irq_ok) == KDI_OK, "sensor token never-expire accepted");
  token_sensor = sensor_ttl;
}

static void test_driver_lifecycle(void)
{
  KdiIrqRequest irq_ok = {.irqn = 37, .priority = 3};
  KdiCapToken old = token_uart;
  KdiCapToken rotated = KDI_CAP_INVALID;
  KdiStats stats = {0};

  kdi_set_now_ms(200000U);
  check(kdi_rotate_token(KDI_DRIVER_UART, &rotated) == KDI_OK, "refresh uart token before lifecycle");
  token_uart = rotated;
  old = token_uart;

  check(kdi_driver_runtime_error(KDI_DRIVER_UART, token_uart, 0x55U) == KDI_OK, "runtime error enters error state");
  check_state(KDI_DRIVER_UART, KDI_STATE_ERROR, "uart state is error");

  check(kdi_driver_reset(KDI_DRIVER_UART, token_kernel) == KDI_OK, "kernel reset moves error to reset");
  check_state(KDI_DRIVER_UART, KDI_STATE_RESET, "uart state is reset");

  check(kdi_driver_reinit(KDI_DRIVER_UART, token_kernel, &rotated) == KDI_OK, "kernel reinit from reset");
  check(rotated != KDI_CAP_INVALID, "reinit token non-zero");
  check(rotated != old, "reinit rotates token");
  token_uart = rotated;
  check_state(KDI_DRIVER_UART, KDI_STATE_INIT, "uart state reinit to init");

  check(kdi_request_irq(KDI_DRIVER_UART, token_uart, &irq_ok) == KDI_ERR_STATE, "init state blocks runtime requests");

  check(kdi_driver_probe(KDI_DRIVER_UART, token_uart) == KDI_OK, "probe start from init");
  check_state(KDI_DRIVER_UART, KDI_STATE_PROBE, "uart state is probe");

  check(kdi_driver_probe_done(KDI_DRIVER_UART, token_uart, 0U) == KDI_OK, "probe fail enters error");
  check_state(KDI_DRIVER_UART, KDI_STATE_ERROR, "uart state probe-fail error");

  check(kdi_driver_reset(KDI_DRIVER_UART, token_kernel) == KDI_OK, "reset after probe fail");
  check(kdi_driver_reinit(KDI_DRIVER_UART, token_kernel, &rotated) == KDI_OK, "reinit after probe fail");
  token_uart = rotated;
  check(kdi_driver_probe(KDI_DRIVER_UART, token_uart) == KDI_OK, "probe restart");
  check(kdi_driver_probe_done(KDI_DRIVER_UART, token_uart, 1U) == KDI_OK, "probe success to ready");
  check_state(KDI_DRIVER_UART, KDI_STATE_READY, "uart state is ready");
  check(kdi_driver_activate(KDI_DRIVER_UART, token_uart) == KDI_OK, "ready to active");
  check_state(KDI_DRIVER_UART, KDI_STATE_ACTIVE, "uart state is active after reinit");

  check(kdi_driver_force_reclaim(KDI_DRIVER_UART, token_kernel) == KDI_OK, "kernel force reclaim");
  check_state(KDI_DRIVER_UART, KDI_STATE_DEAD, "uart state is dead after reclaim");
  check(kdi_request_irq(KDI_DRIVER_UART, token_uart, &irq_ok) == KDI_ERR_AUTH, "reclaimed driver token revoked");

  kdi_get_stats(&stats);
  check(stats.force_reclaim_total >= 1U, "force reclaim stats count");
  check(stats.state_fail_total >= 1U, "state reject stats count");
}

static void test_irq_deferred_concurrency(void)
{
  KdiCapToken refreshed = KDI_CAP_INVALID;
  uint32_t processed = 0U;
  uint32_t detected = 0U;
  uint32_t budget = 0U;
  uint32_t worker_sum = 0U;
  KdiIrqStats irq_stats = {0};
  KdiIrqDriverStats drv_stats = {0};
  KdiIrqDriverCounters ctr_before = {0};
  KdiIrqDriverCounters ctr_after = {0};

  check(kdi_driver_reinit(KDI_DRIVER_UART, token_kernel, &refreshed) == KDI_OK, "reinit uart before irq test");
  token_uart = refreshed;
  check(kdi_driver_probe(KDI_DRIVER_UART, token_uart) == KDI_OK, "probe uart before irq test");
  check(kdi_driver_probe_done(KDI_DRIVER_UART, token_uart, 1U) == KDI_OK, "ready uart before irq test");
  check(kdi_driver_activate(KDI_DRIVER_UART, token_uart) == KDI_OK, "activate uart before irq test");

  kdi_set_now_ms(300000U);
  check(kdi_set_token_ttl_ms(KDI_DRIVER_UART, 0U) == KDI_OK, "uart ttl never expire for irq model");
  check(kdi_rotate_token(KDI_DRIVER_UART, &refreshed) == KDI_OK, "refresh uart token for irq model");
  token_uart = refreshed;
  irq_worker_calls = 0U;
  irq_worker_arg_sum = 0U;
  irq_worker_last_id = 0U;
  check(kdi_irq_set_cooldown_ms(KDI_DRIVER_UART, 200U, 100U) == KDI_ERR_BAD_ARG, "cooldown base>max rejected");
  check(kdi_irq_set_cooldown_ms(KDI_DRIVER_UART, 50U, 200U) == KDI_OK, "set uart cooldown range");
  check(kdi_irq_set_worker(KDI_DRIVER_UART, token_sensor, test_irq_worker, &worker_sum) == KDI_ERR_AUTH,
        "worker registration enforces token");
  check(kdi_irq_set_worker(KDI_DRIVER_UART, token_uart, test_irq_worker, &worker_sum) == KDI_OK,
        "register deferred worker hook");
  check(kdi_irq_get_driver_counters(KDI_DRIVER_UART, &ctr_before) == KDI_OK, "driver irq counters baseline");
  check(kdi_irq_get_driver_stats(KDI_DRIVER_UART, &drv_stats) == KDI_OK, "driver irq stats query after hook");
  check(drv_stats.worker_registered == 1U, "worker hook registered");
  check(drv_stats.cooldown_base_ms == 50U, "driver cooldown base persisted");
  check(drv_stats.cooldown_max_ms == 200U, "driver cooldown max persisted");

  check(kdi_irq_set_budget_per_sec(KDI_DRIVER_UART, 3U) == KDI_OK, "set uart irq budget");
  check(kdi_irq_get_budget_per_sec(KDI_DRIVER_UART, &budget) == KDI_OK, "get uart irq budget");
  check(budget == 3U, "uart irq budget persisted");

  for (uint32_t i = 0U; i < 3U; ++i) {
    check(kdi_irq_enter(KDI_DRIVER_UART, token_uart) == KDI_OK, "irq enter within budget");
    check(kdi_irq_unsafe_op(KDI_DRIVER_UART, token_uart, KDI_IRQ_UNSAFE_MALLOC) == KDI_ERR_STATE, "malloc forbidden in irq");
    check(kdi_irq_unsafe_op(KDI_DRIVER_UART, token_uart, KDI_IRQ_UNSAFE_PRINTF) == KDI_ERR_STATE, "printf forbidden in irq");
    check(kdi_irq_unsafe_op(KDI_DRIVER_UART, token_uart, KDI_IRQ_UNSAFE_POLICY) == KDI_ERR_STATE, "policy change forbidden in irq");
    check(kdi_irq_defer(KDI_DRIVER_UART, token_uart, (uint16_t)(0x10U + i), i) == KDI_OK, "defer work from irq");
    check(kdi_irq_exit(KDI_DRIVER_UART, token_uart) == KDI_OK, "irq exit");
  }

  check(kdi_irq_enter(KDI_DRIVER_UART, token_uart) == KDI_ERR_LIMIT, "irq storm throttled by budget");
  check(kdi_irq_get_driver_stats(KDI_DRIVER_UART, &drv_stats) == KDI_OK, "driver irq stats query");
  check(drv_stats.throttled == 1U, "driver marked throttled");
  check(drv_stats.cooldown_level >= 1U, "cooldown level raised after storm");
  check(drv_stats.cooldown_until_ms >= 300050U, "cooldown deadline set");

  kdi_set_now_ms(300010U);
  check(kdi_irq_enter(KDI_DRIVER_UART, token_uart) == KDI_ERR_LIMIT, "cooldown blocks immediate re-enter");
  kdi_set_now_ms(300070U);
  check(kdi_irq_enter(KDI_DRIVER_UART, token_uart) == KDI_OK, "cooldown auto-recovers without waiting full second");
  check(kdi_irq_exit(KDI_DRIVER_UART, token_uart) == KDI_OK, "irq exit after cooldown recover");

  check(kdi_irq_worker_run(2U, &processed) == KDI_OK, "run deferred worker");
  check(processed == 2U, "worker processed budgeted amount");
  check(irq_worker_calls == 2U, "worker hook called for processed items");
  check(irq_worker_last_id == 0x11U, "worker hook preserves FIFO order");
  check(irq_worker_arg_sum == 1U, "worker hook accumulates args");
  check(worker_sum == irq_worker_arg_sum, "worker hook ctx observed args");
  check(kdi_irq_get_driver_stats(KDI_DRIVER_UART, &drv_stats) == KDI_OK, "driver irq stats query after worker");
  check(drv_stats.deferred_pending >= 1U, "deferred backlog remains");

  check(kdi_irq_set_starvation_ms(10U) == KDI_OK, "set starvation threshold");
  kdi_set_now_ms(300200U);
  check(kdi_irq_poll_starvation(&detected) == KDI_OK, "starvation poll");
  check(detected == 1U, "starvation detected on queued work");

  check(kdi_irq_worker_run(16U, &processed) == KDI_OK, "drain deferred queue");
  check(processed >= 1U, "worker drained backlog");
  check(irq_worker_calls == 3U, "worker hook drained remaining queue item");
  check(irq_worker_last_id == 0x12U, "worker hook saw final backlog item");
  check(irq_worker_arg_sum == 3U, "worker hook sum after backlog drain");

  kdi_set_now_ms(301300U);
  check(kdi_irq_enter(KDI_DRIVER_UART, token_uart) == KDI_OK, "budget window resets after 1s");
  check(kdi_irq_defer(KDI_DRIVER_UART, token_uart, 0x21U, 7U) == KDI_OK, "defer after window reset");
  check(kdi_irq_exit(KDI_DRIVER_UART, token_uart) == KDI_OK, "irq exit after reset");
  check(kdi_irq_worker_run(8U, &processed) == KDI_OK, "worker flush post-reset");
  check(irq_worker_calls == 4U, "worker hook called after window reset");
  check(irq_worker_last_id == 0x21U, "worker hook saw post-reset work id");
  check(irq_worker_arg_sum == 10U, "worker hook sum after post-reset item");

  kdi_set_now_ms(302400U);
  check(kdi_irq_enter(KDI_DRIVER_UART, token_uart) == KDI_OK, "enter irq for worker error path");
  check(kdi_irq_defer(KDI_DRIVER_UART, token_uart, 0xEEU, 5U) == KDI_OK, "queue worker error sentinel");
  check(kdi_irq_exit(KDI_DRIVER_UART, token_uart) == KDI_OK, "irq exit for worker error path");
  check(kdi_irq_worker_run(8U, &processed) == KDI_OK, "worker run for error sentinel");
  check(irq_worker_calls == 5U, "worker hook invoked for error sentinel");
  check(irq_worker_last_id == 0xEEU, "worker hook saw error sentinel id");
  check(kdi_irq_set_worker(KDI_DRIVER_UART, token_uart, NULL, NULL) == KDI_OK, "disable worker hook");
  check(kdi_irq_get_driver_stats(KDI_DRIVER_UART, &drv_stats) == KDI_OK, "driver irq stats query after hook clear");
  check(drv_stats.worker_registered == 0U, "worker hook cleared");
  check(kdi_irq_get_driver_counters(KDI_DRIVER_UART, &ctr_after) == KDI_OK, "driver irq counters after workload");
  check(ctr_after.irq_enter_total >= ctr_before.irq_enter_total + 6U, "driver enter counters accumulate");
  check(ctr_after.irq_throttle_total >= ctr_before.irq_throttle_total + 1U, "driver throttle counters accumulate");
  check(ctr_after.irq_defer_total >= ctr_before.irq_defer_total + 5U, "driver defer counters accumulate");
  check(ctr_after.irq_worker_total >= ctr_before.irq_worker_total + 5U, "driver worker counters accumulate");
  check(ctr_after.irq_worker_error_total >= ctr_before.irq_worker_error_total + 1U,
        "driver worker error counters accumulate");
  check(ctr_after.irq_unsafe_total >= ctr_before.irq_unsafe_total + 3U, "driver unsafe counters accumulate");

  kdi_irq_get_stats(&irq_stats);
  check(irq_stats.irq_enter_total >= 5U, "irq enter stats count");
  check(irq_stats.irq_throttle_total >= 1U, "irq throttle stats count");
  check(irq_stats.irq_cooldown_total >= 1U, "irq cooldown stats count");
  check(irq_stats.irq_recover_total >= 1U, "irq recover stats count");
  check(irq_stats.irq_defer_total >= 5U, "irq defer stats count");
  check(irq_stats.irq_worker_total >= 5U, "irq worker stats count");
  check(irq_stats.irq_worker_error_total >= 1U, "irq worker error stats count");
  check(irq_stats.irq_starvation_total >= 1U, "irq starvation stats count");
  check(irq_stats.irq_unsafe_total >= 3U, "irq unsafe stats count");
}

static void test_driver_profile_stats(void)
{
  KdiDriverProfileStats uart_before = {0};
  KdiDriverProfileStats uart_after = {0};
  KdiDriverProfileStats sensor_before = {0};
  KdiDriverProfileStats sensor_after = {0};
  KdiIrqRequest irq_ok = {.irqn = 39, .priority = 3};
  KdiMpuRequest mpu = {
    .region_index = 1U,
    .base = 0x20004000U,
    .size = 256U,
    .attrs = 0U,
  };
  KdiCapToken rotated = KDI_CAP_INVALID;

  check(kdi_profile_get_driver(KDI_DRIVER_UART, &uart_before) == KDI_OK, "profile query uart before");
  check(kdi_profile_get_driver(KDI_DRIVER_SENSOR, &sensor_before) == KDI_OK, "profile query sensor before");

  check(kdi_request_irq(KDI_DRIVER_UART, token_uart, &irq_ok) == KDI_OK, "profile irq success sample");
  check(kdi_request_irq(KDI_DRIVER_UART, KDI_CAP_INVALID, &irq_ok) == KDI_ERR_AUTH, "profile irq fail sample");
  check(kdi_request_mpu_region(KDI_DRIVER_SENSOR, token_sensor, &mpu) == KDI_ERR_DENIED,
        "profile mpu denied sample");

  check(kdi_driver_runtime_error(KDI_DRIVER_UART, token_uart, 0x66U) == KDI_OK, "profile transition active->error");
  check(kdi_driver_reset(KDI_DRIVER_UART, token_kernel) == KDI_OK, "profile transition error->reset");
  check(kdi_driver_reinit(KDI_DRIVER_UART, token_kernel, &rotated) == KDI_OK, "profile transition reset->init");
  token_uart = rotated;
  check(kdi_driver_probe(KDI_DRIVER_UART, token_uart) == KDI_OK, "profile transition init->probe");
  check(kdi_driver_probe_done(KDI_DRIVER_UART, token_uart, 1U) == KDI_OK, "profile transition probe->ready");
  check(kdi_driver_activate(KDI_DRIVER_UART, token_uart) == KDI_OK, "profile transition ready->active");

  check(kdi_profile_get_driver(KDI_DRIVER_UART, &uart_after) == KDI_OK, "profile query uart after");
  check(kdi_profile_get_driver(KDI_DRIVER_SENSOR, &sensor_after) == KDI_OK, "profile query sensor after");

  check(uart_after.request_total[KDI_REQ_IRQ] >= uart_before.request_total[KDI_REQ_IRQ] + 2U,
        "profile irq request totals accumulate");
  check(uart_after.request_fail[KDI_REQ_IRQ] >= uart_before.request_fail[KDI_REQ_IRQ] + 1U,
        "profile irq fail totals accumulate");
  check(sensor_after.request_total[KDI_REQ_MPU] >= sensor_before.request_total[KDI_REQ_MPU] + 1U,
        "profile mpu request totals accumulate");
  check(sensor_after.request_fail[KDI_REQ_MPU] >= sensor_before.request_fail[KDI_REQ_MPU] + 1U,
        "profile mpu fail totals accumulate");

  check(uart_after.state_transition[KDI_STATE_ACTIVE][KDI_STATE_ERROR] >=
          uart_before.state_transition[KDI_STATE_ACTIVE][KDI_STATE_ERROR] + 1U,
        "profile tracks active->error transitions");
  check(uart_after.state_transition[KDI_STATE_ERROR][KDI_STATE_RESET] >=
          uart_before.state_transition[KDI_STATE_ERROR][KDI_STATE_RESET] + 1U,
        "profile tracks error->reset transitions");
  check(uart_after.state_transition[KDI_STATE_RESET][KDI_STATE_INIT] >=
          uart_before.state_transition[KDI_STATE_RESET][KDI_STATE_INIT] + 1U,
        "profile tracks reset->init transitions");
  check(uart_after.state_transition[KDI_STATE_INIT][KDI_STATE_PROBE] >=
          uart_before.state_transition[KDI_STATE_INIT][KDI_STATE_PROBE] + 1U,
        "profile tracks init->probe transitions");
  check(uart_after.state_transition[KDI_STATE_PROBE][KDI_STATE_READY] >=
          uart_before.state_transition[KDI_STATE_PROBE][KDI_STATE_READY] + 1U,
        "profile tracks probe->ready transitions");
  check(uart_after.state_transition[KDI_STATE_READY][KDI_STATE_ACTIVE] >=
          uart_before.state_transition[KDI_STATE_READY][KDI_STATE_ACTIVE] + 1U,
        "profile tracks ready->active transitions");
  check(uart_after.state_visit[KDI_STATE_ACTIVE] >= uart_before.state_visit[KDI_STATE_ACTIVE] + 1U,
        "profile tracks state visits");
}

static void test_cap_usage_trace(void)
{
  KdiCapUsageTrace uart = {0};
  KdiCapUsageTrace vm = {0};

  check(kdi_cap_usage_get(KDI_DRIVER_UART, &uart) == KDI_OK, "cap usage query uart");
  check(kdi_cap_usage_get(KDI_DRIVER_VM_RUNTIME, &vm) == KDI_OK, "cap usage query vm");
  check(kdi_cap_usage_get(KDI_DRIVER_COUNT, &uart) == KDI_ERR_BAD_ARG, "cap usage bad driver");
  check(kdi_cap_usage_get(KDI_DRIVER_UART, NULL) == KDI_ERR_BAD_ARG, "cap usage null out");

  check((uart.declared_mask & KDI_CAP_REQ_BIT(KDI_REQ_IRQ)) != 0U, "uart declares irq capability");
  check((uart.declared_mask & KDI_CAP_REQ_BIT(KDI_REQ_MPU)) == 0U, "uart does not declare mpu capability");
  check((uart.used_mask & KDI_CAP_REQ_BIT(KDI_REQ_IRQ)) != 0U, "uart irq capability used");
  check((uart.active_mask & KDI_CAP_REQ_BIT(KDI_REQ_IRQ)) != 0U, "uart irq capability active use");
  check((uart.declared_not_used_mask & uart.used_mask) == 0U, "declared-not-used is disjoint from used");
  check((uart.declared_not_used_mask & (~uart.declared_mask)) == 0U, "declared-not-used is subset of declared");
  check(uart.declared_not_used_mask != 0U, "uart has declared capabilities that remain unused");
  check(uart.observation_end_ms >= uart.observation_start_ms, "cap usage observation window monotonic");
  check(uart.observation_window_ms == (uart.observation_end_ms - uart.observation_start_ms),
        "cap usage observation window duration");
  check(uart.request_last_ms[KDI_REQ_IRQ] >= uart.request_first_ms[KDI_REQ_IRQ],
        "uart irq usage window tracked");
  check(uart.request_total[KDI_REQ_POWER] == 0U &&
          uart.request_first_ms[KDI_REQ_POWER] == 0U &&
          uart.request_last_ms[KDI_REQ_POWER] == 0U,
        "unused capability keeps zero usage window");

  check((vm.declared_mask & KDI_CAP_REQ_BIT(KDI_REQ_MPU)) != 0U, "vm declares mpu capability");
  check((vm.used_mask & KDI_CAP_REQ_BIT(KDI_REQ_MPU)) != 0U, "vm mpu capability used");

  for (uint32_t req = (uint32_t)KDI_REQ_MPU; req <= (uint32_t)KDI_REQ_RESET; ++req) {
    check(uart.request_ok[req] + uart.request_fail[req] == uart.request_total[req],
          "uart cap usage counters conserve total");
  }
}

int main(void)
{
  kdi_init();
  check(kdi_acquire_token(KDI_DRIVER_KERNEL, &token_kernel) == KDI_OK, "acquire kernel token");
  check(kdi_acquire_token(KDI_DRIVER_UART, &token_uart) == KDI_OK, "acquire uart token");
  check(kdi_acquire_token(KDI_DRIVER_SENSOR, &token_sensor) == KDI_OK, "acquire sensor token");
  check(kdi_acquire_token(KDI_DRIVER_VM_RUNTIME, &token_vm) == KDI_OK, "acquire vm token");
  check(kdi_acquire_token(KDI_DRIVER_DIAG, &token_diag) == KDI_OK, "acquire diag token");
  check(token_kernel != KDI_CAP_INVALID, "kernel token non-zero");
  check(token_uart != KDI_CAP_INVALID, "uart token non-zero");
  check(token_sensor != KDI_CAP_INVALID, "sensor token non-zero");
  check(token_vm != KDI_CAP_INVALID, "vm token non-zero");
  check(token_diag != KDI_CAP_INVALID, "diag token non-zero");
  check_state(KDI_DRIVER_KERNEL, KDI_STATE_ACTIVE, "kernel starts active");

  boot_driver_to_active(KDI_DRIVER_UART, token_uart, "uart");
  boot_driver_to_active(KDI_DRIVER_SENSOR, token_sensor, "sensor");
  boot_driver_to_active(KDI_DRIVER_VM_RUNTIME, token_vm, "vm");
  boot_driver_to_active(KDI_DRIVER_DIAG, token_diag, "diag");

  test_policy_matrix();
  test_mpu_contract();
  test_irq_and_dma_contract();
  test_dma_zero_copy_lifecycle();
  test_fault_hook_and_stats();
  test_fault_domain_containment();
  test_last_decision();
  test_token_contract();
  test_token_lifecycle();
  test_token_ttl_expiry();
  test_driver_lifecycle();
  test_irq_deferred_concurrency();
  test_driver_profile_stats();
  test_cap_usage_trace();

  if (failures == 0) {
    printf("kdi_host_tests: OK\n");
    return 0;
  }
  printf("kdi_host_tests: FAIL (%d)\n", failures);
  return 1;
}
