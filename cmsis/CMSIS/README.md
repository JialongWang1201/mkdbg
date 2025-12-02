Phase 1a note

This repository vendors CMSIS headers used by the STM32F446 firmware build.

Phase 1a keeps this directory read-only while host-side mkdbg-native refactoring
proceeds in `tools/`. The intent is to isolate build-system and host-tool
changes from MCU register and core header dependencies.
