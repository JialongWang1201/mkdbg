Phase 1a note

This directory owns linker scripts for the firmware image layout.

Phase 1a keeps linker inputs unchanged while CMake and host-tool refactors are
landed. Preserving memory layout avoids coupling host-side cleanup to firmware
image placement.
