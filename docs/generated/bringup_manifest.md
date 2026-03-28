# Bringup Manifest

Generated from `examples/stm32f446/configs/bringup/manifest.yaml` by `tools/bringup_compile.py`.

This document is the exported view of the declarative bringup manifest that drives:

- `bringup stage wait|wait-json` stage -> driver -> resource triage
- `dep whatif|whatif-json` static impact projection
- generated C tables committed in `include/bringup_manifest_gen.h`

## Summary

- phases: 9
- stages: 5
- driver dependency edges: 5
- resource dependency edges: 8
- stage-driver edges: 5

## Phases

| Enum | Name | Stage | Aliases |
| --- | --- | --- | --- |
| BRINGUP_PHASE_ROM_EARLY_INIT | rom-early-init | BRINGUP_STAGE_INIT | rom, early |
| BRINGUP_PHASE_MPU_SETUP | mpu-setup | BRINGUP_STAGE_MPU | mpu |
| BRINGUP_PHASE_KERNEL_START | kernel-start | BRINGUP_STAGE_KERNEL | kernel |
| BRINGUP_PHASE_DRIVER_PROBE_DIAG | driver-probe-diag | BRINGUP_STAGE_DRIVERS | driver, probe, driver-probe, driver-diag, diag |
| BRINGUP_PHASE_DRIVER_PROBE_UART | driver-probe-uart | BRINGUP_STAGE_DRIVERS | driver-uart, uart |
| BRINGUP_PHASE_DRIVER_PROBE_SENSOR | driver-probe-sensor | BRINGUP_STAGE_DRIVERS | driver-sensor, sensor |
| BRINGUP_PHASE_DRIVER_PROBE_VM | driver-probe-vm | BRINGUP_STAGE_DRIVERS | driver-vm, vm |
| BRINGUP_PHASE_SERVICE_REGISTRATION | service-registration | BRINGUP_STAGE_DRIVERS | service, services |
| BRINGUP_PHASE_USER_WORKLOAD_ENABLE | user-workload-enable | BRINGUP_STAGE_READY | user, workload |

## Stages

| Enum | Name | Phase Range | Entry Event | Exit Event | Aliases |
| --- | --- | --- | --- | --- | --- |
| BRINGUP_STAGE_INIT | init | BRINGUP_PHASE_ROM_EARLY_INIT -> BRINGUP_PHASE_ROM_EARLY_INIT | bringup.stage.init.enter | bringup.stage.init.exit | - |
| BRINGUP_STAGE_MPU | mpu | BRINGUP_PHASE_MPU_SETUP -> BRINGUP_PHASE_MPU_SETUP | bringup.stage.mpu.enter | bringup.stage.mpu.exit | - |
| BRINGUP_STAGE_KERNEL | kernel | BRINGUP_PHASE_KERNEL_START -> BRINGUP_PHASE_KERNEL_START | bringup.stage.kernel.enter | bringup.stage.kernel.exit | - |
| BRINGUP_STAGE_DRIVERS | drivers | BRINGUP_PHASE_DRIVER_PROBE_DIAG -> BRINGUP_PHASE_SERVICE_REGISTRATION | bringup.stage.drivers.enter | bringup.stage.drivers.exit | driver |
| BRINGUP_STAGE_READY | ready | BRINGUP_PHASE_USER_WORKLOAD_ENABLE -> BRINGUP_PHASE_USER_WORKLOAD_ENABLE | bringup.stage.ready.enter | bringup.stage.ready.exit | - |

## Driver Edges

| From | To | Reason |
| --- | --- | --- |
| KDI_DRIVER_UART | KDI_DRIVER_DIAG | diagnostic log/control path |
| KDI_DRIVER_SENSOR | KDI_DRIVER_UART | sensor telemetry uplink |
| KDI_DRIVER_VM_RUNTIME | KDI_DRIVER_UART | vm io service channel |
| KDI_DRIVER_VM_RUNTIME | KDI_DRIVER_SENSOR | vm consumes sensor data plane |
| KDI_DRIVER_VM_RUNTIME | KDI_DRIVER_DIAG | vm fault trace routing |

## Resource Edges

| Driver | Kind | Resource | Reason |
| --- | --- | --- | --- |
| KDI_DRIVER_UART | DEP_RESOURCE_IRQ | USARTx_VCP | uart rx/tx interrupt path |
| KDI_DRIVER_UART | DEP_RESOURCE_MEMORY | g_shared_ctrl | runtime knob sync |
| KDI_DRIVER_SENSOR | DEP_RESOURCE_DMA | ADC_DMA_RING | adc dma sample ingress |
| KDI_DRIVER_SENSOR | DEP_RESOURCE_MEMORY | g_shared_adc | sensor sample window |
| KDI_DRIVER_VM_RUNTIME | DEP_RESOURCE_MEMORY | g_shared_adc | vm sensor read mapping |
| KDI_DRIVER_VM_RUNTIME | DEP_RESOURCE_MEMORY | g_shared_ctrl | vm control mapping |
| KDI_DRIVER_VM_RUNTIME | DEP_RESOURCE_MEMORY | g_shared_stats | vm stats mapping |
| KDI_DRIVER_DIAG | DEP_RESOURCE_MEMORY | log_queue | diagnostic sink buffer |

## Stage Driver Edges

| Phase | Driver | Reason |
| --- | --- | --- |
| BRINGUP_PHASE_KERNEL_START | KDI_DRIVER_KERNEL | kernel runtime online |
| BRINGUP_PHASE_DRIVER_PROBE_DIAG | KDI_DRIVER_DIAG | diag probe/activate |
| BRINGUP_PHASE_DRIVER_PROBE_UART | KDI_DRIVER_UART | uart probe/activate |
| BRINGUP_PHASE_DRIVER_PROBE_SENSOR | KDI_DRIVER_SENSOR | sensor probe/activate |
| BRINGUP_PHASE_DRIVER_PROBE_VM | KDI_DRIVER_VM_RUNTIME | vm probe/activate |
