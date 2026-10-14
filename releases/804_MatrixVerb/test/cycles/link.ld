MEMORY { RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 0x70000 }
ENTRY(_start)
SECTIONS {
  .text : { KEEP(*(.text.start)) *(.text*) *(.rodata*) . = ALIGN(4); } > RAM
  .ARM.exidx : { *(.ARM.exidx*) } > RAM
  .data : { *(.data*) . = ALIGN(4); } > RAM
  .bss (NOLOAD) : { __bss_start__ = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end__ = .; } > RAM
  end = .; _end = .;
}
