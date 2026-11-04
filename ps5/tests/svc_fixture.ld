/* SPDX-License-Identifier: GPL-3.0-or-later */
ENTRY(svc_clock)
PHDRS { code PT_LOAD FLAGS(5); }
SECTIONS {
    . = 0x7300000000;
    .text : { *(.text*) } :code
    /DISCARD/ : { *(.comment) }
}
