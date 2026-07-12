#ifndef CAPTURE_TRANSPORT_H
#define CAPTURE_TRANSPORT_H

#include "transport.h"

/* Takes ownership of inner. Returns NULL after closing inner on failure. */
WireTransport *capture_transport_wrap(WireTransport *inner, const char *path,
                                      const char *arch_name);

/* Opens a deterministic transport backed by a previously recorded capture. */
WireTransport *capture_replay_transport_open(const char *path,
                                             const char *expected_arch);

#endif /* CAPTURE_TRANSPORT_H */
