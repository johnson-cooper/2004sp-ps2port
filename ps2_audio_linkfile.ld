/*
 * PS2SDK EE default linkfile with one isolated high-memory audio overlay.
 *
 * Normal client sections intentionally retain the stock ordering/addresses.
 * The companion-loader code/data is put in a second PT_LOAD at 0x01fc0000.
 * _end remains the end of the normal BSS, while _heap_size is explicitly
 * capped so the heap can never grow into the overlay. The top 128 KiB remains
 * available for the stock PS2SDK stack reservation.
 */
ENTRY(__start);

PHDRS {
  text PT_LOAD;
  audio PT_LOAD FLAGS(7);
}

SECTIONS {
    .text 0x00100000: {
        _ftext = . ;
        *(.text)
        *(.text.*)
        *(.gnu.linkonce.t*)
        KEEP(*(.init))
        KEEP(*(.fini))
        QUAD(0)
    } :text

    PROVIDE(_etext = .);
    PROVIDE(etext = .);

    .ctors ALIGN(16): {
        KEEP(*crtbegin*.o(.ctors))
        KEEP(*(EXCLUDE_FILE(*crtend*.o) .ctors))
        KEEP(*(SORT(.ctors.*)))
        KEEP(*(.ctors))
    }

    .dtors ALIGN(16): {
        KEEP(*crtbegin*.o(.dtors))
        KEEP(*(EXCLUDE_FILE(*crtend*.o) .dtors))
        KEEP(*(SORT(.dtors.*)))
        KEEP(*(.dtors))
    }

    .reginfo : { *(.reginfo) }

    .data ALIGN(128): {
        _fdata = . ;
        *(.data)
        *(.data.*)
        *(.gnu.linkonce.d*)
        SORT(CONSTRUCTORS)
    }

    .rodata ALIGN(128): {
        *(.rodata)
        *(.rodata.*)
        *(.gnu.linkonce.r*)
    }

    .rdata ALIGN(128): { *(.rdata) }
    .gcc_except_table ALIGN(128): { *(.gcc_except_table) }

    _gp = ALIGN(128) + 0x7ff0;
    .lit4 ALIGN(128): { *(.lit4) }
    .lit8 ALIGN(128): { *(.lit8) }

    .sdata ALIGN(128): {
        *(.sdata)
        *(.sdata.*)
        *(.gnu.linkonce.s*)
    }

    _edata = .;
    PROVIDE(edata = .);

    .sbss ALIGN(128) : {
        _fbss = . ;
        *(.sbss)
        *(.sbss.*)
        *(.gnu.linkonce.sb*)
        *(.scommon)
    }

    .bss ALIGN(128) : {
        *(.bss)
        *(.bss.*)
        *(.gnu.linkonce.b*)
        *(COMMON)
    }

    _end_bss = .;
    _end = . ;
    PROVIDE(end = .);

    /*
     * Isolated EE shim. Keep this outside the normal client layout entirely.
     * It is loaded by the ELF loader, not cleared by crt0's _fbss.._end loop.
     */
    .ps2_audio_text 0x01fc0000 : {
        KEEP(*(.ps2_audio_text))
        KEEP(*(.ps2_audio_text.*))
    } :audio

    .ps2_audio_rodata ALIGN(64) : {
        KEEP(*(.ps2_audio_rodata))
        KEEP(*(.ps2_audio_rodata.*))
    } :audio

    .ps2_audio_data ALIGN(64) : {
        KEEP(*(.ps2_audio_data))
        KEEP(*(.ps2_audio_data.*))
    } :audio

    _ps2_audio_end = .;

    .spad 0x70000000: {
        *(.spad)
    }

    /* Heap ends at the overlay; stock stack remains the top 128 KiB. */
    PROVIDE(_heap_size = 0x01fc0000 - _end);
    PROVIDE(_stack = -1);
    PROVIDE(_stack_size = 128 * 1024);
}
