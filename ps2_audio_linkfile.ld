/*
 * PS2SDK EE default linkfile with one isolated high-memory audio overlay.
 *
 * Normal client sections intentionally retain the stock ordering/addresses.
 * PS2 driver assets reserve 0x01f90000..0x01fc0000 and the existing
 * companion-loader/audio code begins at 0x01fc0000 in the same high PT_LOAD.
 * _end remains the end of the normal BSS, while _heap_size is explicitly
 * capped so the heap can never grow into either overlay. The top 128 KiB remains
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
        *(EXCLUDE_FILE (*libps2_drivers_local.a:*) .text)
        *(EXCLUDE_FILE (*libps2_drivers_local.a:*) .text.*)
        *(EXCLUDE_FILE (*libps2_drivers_local.a:*) .gnu.linkonce.t*)
        KEEP(*(.init))
        KEEP(*(.fini))
        QUAD(0)

    } :text

    /*
     * Hardware-good client.elf begins .ctors at 0x001c8ef0. The PS2 has
     * repeatedly regressed when audio experiments shifted downstream
     * .data/.sdata/.bss. Code may shrink into an unused gap, but if it grows
     * past the proven boundary the link must fail instead of moving anything.
     */
    ASSERT(ADDR(.text) + SIZEOF(.text) <= 0x001c8ef0,
           "normal .text exceeded hardware-good PS2 layout budget")
    . = 0x001c8ef0;

    PROVIDE(_etext = .);
    PROVIDE(etext = .);

    .ctors ALIGN(16): {
        /*
         * Current PS2Build startup/crt0 walks ctor boundaries explicitly.
         * Keep the legacy section ordering but expose the symbols expected by
         * the active startup library.
         */
        __CTOR_LIST__ = .;
        KEEP(*crtbegin*.o(.ctors))
        KEEP(*(EXCLUDE_FILE(*crtend*.o) .ctors))
        KEEP(*(SORT(.ctors.*)))
        KEEP(*(.ctors))
        __CTOR_END__ = .;
    }

    .dtors ALIGN(16): {
        __DTOR_LIST__ = .;
        KEEP(*crtbegin*.o(.dtors))
        KEEP(*(EXCLUDE_FILE(*crtend*.o) .dtors))
        KEEP(*(SORT(.dtors.*)))
        KEEP(*(.dtors))
        __DTOR_END__ = .;
    }

    .reginfo : { *(.reginfo) }

    .data ALIGN(128): {
        _fdata = . ;
        *(EXCLUDE_FILE (*libps2_drivers_local.a:* *sio2man_irx.o *iomanX_irx.o *fileXio_irx.o *mcman_irx.o *mcserv_irx.o *bdm_irx.o *bdmfs_fatfs_irx.o *usbd_irx.o *usbmass_bd_irx.o *mx4sio_bd_irx.o *cdfs_irx.o *ps2mouse_irx.o *ps2kbd_irx.o *mmceman_embed_irx.o) .data)
        *(EXCLUDE_FILE (*libps2_drivers_local.a:* *sio2man_irx.o *iomanX_irx.o *fileXio_irx.o *mcman_irx.o *mcserv_irx.o *bdm_irx.o *bdmfs_fatfs_irx.o *usbd_irx.o *usbmass_bd_irx.o *mx4sio_bd_irx.o *cdfs_irx.o *ps2mouse_irx.o *ps2kbd_irx.o *mmceman_embed_irx.o) .data.*)
        *(EXCLUDE_FILE (*libps2_drivers_local.a:* *sio2man_irx.o *iomanX_irx.o *fileXio_irx.o *mcman_irx.o *mcserv_irx.o *bdm_irx.o *bdmfs_fatfs_irx.o *usbd_irx.o *usbmass_bd_irx.o *mx4sio_bd_irx.o *cdfs_irx.o *ps2mouse_irx.o *ps2kbd_irx.o *mmceman_embed_irx.o) .gnu.linkonce.d*)
        SORT(CONSTRUCTORS)
    }

    .rodata ALIGN(128): {
        *(EXCLUDE_FILE (*libps2_drivers_local.a:* *sio2man_irx.o *iomanX_irx.o *fileXio_irx.o *mcman_irx.o *mcserv_irx.o *bdm_irx.o *bdmfs_fatfs_irx.o *usbd_irx.o *usbmass_bd_irx.o *mx4sio_bd_irx.o *cdfs_irx.o *ps2mouse_irx.o *ps2kbd_irx.o *mmceman_embed_irx.o *platform/ps2.o) .rodata)
        *(EXCLUDE_FILE (*libps2_drivers_local.a:* *sio2man_irx.o *iomanX_irx.o *fileXio_irx.o *mcman_irx.o *mcserv_irx.o *bdm_irx.o *bdmfs_fatfs_irx.o *usbd_irx.o *usbmass_bd_irx.o *mx4sio_bd_irx.o *cdfs_irx.o *ps2mouse_irx.o *ps2kbd_irx.o *mmceman_embed_irx.o *platform/ps2.o) .rodata.*)
        *(EXCLUDE_FILE (*libps2_drivers_local.a:* *sio2man_irx.o *iomanX_irx.o *fileXio_irx.o *mcman_irx.o *mcserv_irx.o *bdm_irx.o *bdmfs_fatfs_irx.o *usbd_irx.o *usbmass_bd_irx.o *mx4sio_bd_irx.o *cdfs_irx.o *ps2mouse_irx.o *ps2kbd_irx.o *mmceman_embed_irx.o *platform/ps2.o) .gnu.linkonce.r*)

        /*
         * Preserve the exact hardware-good .rodata extent. If real content grows
         * past this address, ld will reject the attempted backwards move instead
         * of shifting .MIPS.abiflags/.sdata/.bss.
         */
        . = 0x0000cbe0;
    }

    ASSERT(ADDR(.rodata) == 0x001f3e80,
           "normal .rodata start moved from hardware-good PS2 layout")
    ASSERT(SIZEOF(.rodata) == 0x0000cbe0,
           "normal .rodata size moved from hardware-good PS2 layout")

    .rdata ALIGN(128): { *(.rdata) }
    .gcc_except_table ALIGN(128): { *(.gcc_except_table) }

    _gp = ALIGN(128) + 0x7ff0;
    .lit4 ALIGN(128): { *(.lit4) }
    .lit8 ALIGN(128): { *(.lit8) }

    .sdata ALIGN(128): {
        *(EXCLUDE_FILE (*libps2_drivers_local.a:* *sio2man_irx.o *iomanX_irx.o *fileXio_irx.o *mcman_irx.o *mcserv_irx.o *bdm_irx.o *bdmfs_fatfs_irx.o *usbd_irx.o *usbmass_bd_irx.o *mx4sio_bd_irx.o *cdfs_irx.o *ps2mouse_irx.o *ps2kbd_irx.o *mmceman_embed_irx.o) .sdata)
        *(EXCLUDE_FILE (*libps2_drivers_local.a:* *sio2man_irx.o *iomanX_irx.o *fileXio_irx.o *mcman_irx.o *mcserv_irx.o *bdm_irx.o *bdmfs_fatfs_irx.o *usbd_irx.o *usbmass_bd_irx.o *mx4sio_bd_irx.o *cdfs_irx.o *ps2mouse_irx.o *ps2kbd_irx.o *mmceman_embed_irx.o) .sdata.*)
        *(EXCLUDE_FILE (*libps2_drivers_local.a:* *sio2man_irx.o *iomanX_irx.o *fileXio_irx.o *mcman_irx.o *mcserv_irx.o *bdm_irx.o *bdmfs_fatfs_irx.o *usbd_irx.o *usbmass_bd_irx.o *mx4sio_bd_irx.o *cdfs_irx.o *ps2mouse_irx.o *ps2kbd_irx.o *mmceman_embed_irx.o) .gnu.linkonce.s*)
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
     * Vendored ps2_drivers code, all storage/HID IRX byte arrays used by it, and platform/ps2.o
     * read-only constants live in a dedicated high-memory PT_LOAD. Moving ps2.o rodata here keeps
     * new boot/HID diagnostics from consuming the hardware-proven low .rodata budget while its
     * executable code/data remain in their original placement. DEV9/NETMAN/SMAP and audsrv remain
     * in their proven placement.
     *
     * 0x01f90000..0x01fc0000 reserves 192 KiB for this overlay. The existing audio overlay
     * keeps its exact 0x01fc0000 address.
     */
    .ps2_driver_overlay 0x01f90000 : {
        lib/libps2_drivers_local.a:*(.text .text.* .gnu.linkonce.t*)
        lib/libps2_drivers_local.a:*(.rodata .rodata.* .gnu.linkonce.r*)
        *platform/ps2.o(.rodata .rodata.* .gnu.linkonce.r*)
        lib/libps2_drivers_local.a:*(.data .data.* .gnu.linkonce.d*)
        lib/libps2_drivers_local.a:*(.sdata .sdata.* .gnu.linkonce.s*)

        *sio2man_irx.o(.data .data.* .rodata .rodata.* .sdata .sdata.*)
        *iomanX_irx.o(.data .data.* .rodata .rodata.* .sdata .sdata.*)
        *fileXio_irx.o(.data .data.* .rodata .rodata.* .sdata .sdata.*)
        *mcman_irx.o(.data .data.* .rodata .rodata.* .sdata .sdata.*)
        *mcserv_irx.o(.data .data.* .rodata .rodata.* .sdata .sdata.*)
        *bdm_irx.o(.data .data.* .rodata .rodata.* .sdata .sdata.*)
        *bdmfs_fatfs_irx.o(.data .data.* .rodata .rodata.* .sdata .sdata.*)
        *usbd_irx.o(.data .data.* .rodata .rodata.* .sdata .sdata.*)
        *usbmass_bd_irx.o(.data .data.* .rodata .rodata.* .sdata .sdata.*)
        *mx4sio_bd_irx.o(.data .data.* .rodata .rodata.* .sdata .sdata.*)
        *cdfs_irx.o(.data .data.* .rodata .rodata.* .sdata .sdata.*)
        *ps2mouse_irx.o(.data .data.* .rodata .rodata.* .sdata .sdata.*)
        *ps2kbd_irx.o(.data .data.* .rodata .rodata.* .sdata .sdata.*)
        *mmceman_embed_irx.o(.data .data.* .rodata .rodata.* .sdata .sdata.*)
    } :audio

    _ps2_driver_end = .;
    ASSERT(_ps2_driver_end <= 0x01fc0000,
           "PS2 driver overlay exceeded 192 KiB reserved region")

    /*
     * Isolated EE shim. Keep this outside the normal client layout entirely.
     * It is loaded by the ELF loader, not cleared by crt0's _fbss.._end loop.
     */
    .ps2_audio_text 0x01fc0000 : {
        KEEP(*(.ps2_audio_text))
        KEEP(*(.ps2_audio_text.*))
    } :audio

    /*
     * PS2-only gameplay/runtime helpers may live here when adding them to normal
     * .text would move the hardware-sensitive client layout past its proven
     * boundary. Keep this executable and inside the same high-memory PT_LOAD;
     * unlike the audio state, these functions own no persistent high-memory data.
     */
    .ps2_runtime_text ALIGN(64) : {
        KEEP(*(.ps2_runtime_text))
        KEEP(*(.ps2_runtime_text.*))
    } :audio

    .ps2_runtime_rodata ALIGN(64) : {
        KEEP(*(.ps2_runtime_rodata))
        KEEP(*(.ps2_runtime_rodata.*))
    } :audio

    /*
     * Initialized renderer experiment state belongs in the isolated runtime overlay too. Keeping
     * even its tiny pointer/counter block out of normal .data/.bss preserves the hardware-proven
     * networking-sensitive client layout. The actual triangle queue is allocated lazily post-login.
     */
    .ps2_runtime_data ALIGN(64) : {
        KEEP(*(.ps2_runtime_data))
        KEEP(*(.ps2_runtime_data.*))
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

    /* Heap ends at the driver overlay; stock stack remains the top 128 KiB. */
    PROVIDE(_heap_size = 0x01f90000 - _end);
    PROVIDE(_stack = -1);
    PROVIDE(_stack_size = 128 * 1024);
}
