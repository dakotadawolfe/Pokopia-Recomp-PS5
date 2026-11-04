/* SPDX-License-Identifier: GPL-3.0-or-later */
ENTRY(load_acquire)
PHDRS { code PT_LOAD FLAGS(5); }
SECTIONS {
    . = 0x7100000000;
    .text : { *(.text*) } :code
    /DISCARD/ : { *(.comment) }
}
