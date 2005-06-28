/*
 * (C) Copyright 1992, ..., 2005 the "DOSEMU-Development-Team".
 *
 * for details see file COPYING in the DOSEMU distribution
 */

/* 	MS-DOS API translator for DOSEMU\'s DPMI Server
 *
 * DANG_BEGIN_MODULE msdos.c
 *
 * REMARK
 * MS-DOS API translator allows DPMI programs to call DOS service directly
 * in protected mode.
 *
 * /REMARK
 * DANG_END_MODULE
 *
 * First Attempted by Dong Liu,  dliu@rice.njit.edu
 *
 */

#include <stdlib.h>
#include <string.h>

#include "emu.h"
#include "emu-ldt.h"
#include "int.h"
#include "bios.h"
#include "emm.h"
#include "dpmi.h"
#include "msdos.h"
#include "vgaemu.h"

#define TRANS_BUFFER_SEG EMM_SEGMENT

#define DTA_over_1MB (void*)(GetSegmentBaseAddress(MSDOS_CLIENT.user_dta_sel) + MSDOS_CLIENT.user_dta_off)
#define DTA_under_1MB (void*)((MSDOS_CLIENT.lowmem_seg + DTA_Para_ADD) << 4)

#define MAX_DOS_PATH 260

#define D_16_32(reg)		(MSDOS_CLIENT.is_32 ? reg : reg & 0xffff)

int msdos_client_num = 0;
struct msdos_struct msdos_client[DPMI_MAX_CLIENTS];

void msdos_init(int is_32, unsigned short mseg, unsigned short psp)
{
    msdos_client_num++;
    memset(&MSDOS_CLIENT, 0, sizeof(struct msdos_struct));
    MSDOS_CLIENT.is_32 = is_32;
    MSDOS_CLIENT.lowmem_seg = mseg;
    MSDOS_CLIENT.current_psp = psp;
    MSDOS_CLIENT.current_env_sel = READ_WORD(SEGOFF2LINEAR(psp, 0x2c));
    D_printf("MSDOS: init, %i\n", msdos_client_num);
}

void msdos_done(void)
{
    msdos_client_num--;
    D_printf("MSDOS: done, %i\n", msdos_client_num);
}

int msdos_get_lowmem_size(void)
{
    return DTA_Para_SIZE;
}

static void prepare_ems_frame(void)
{
    if (MSDOS_CLIENT.ems_frame_mapped)
	return;
    MSDOS_CLIENT.ems_frame_mapped = 1;
    emm_get_map_registers(MSDOS_CLIENT.ems_map_buffer);
    emm_unmap_all();
}

static void restore_ems_frame(void)
{
    if (!MSDOS_CLIENT.ems_frame_mapped)
	return;
    emm_set_map_registers(MSDOS_CLIENT.ems_map_buffer);
    MSDOS_CLIENT.ems_frame_mapped = 0;
}

static int need_copy_dseg(struct sigcontext_struct *scp, int intr)
{
    switch (intr) {
	case 0x21:
	    switch (_HI(ax)) {
		case 0x0a:		/* buffered keyboard input */
		case 0x5a:		/* mktemp */
		case 0x69:
		    return 1;
		case 0x44:		/* IOCTL */
		    switch (_LO(ax)) {
			case 0x02 ... 0x05:
			case 0x0c: case 0x0d:
			    return 1;
		    }
		    break;
		case 0x5d:		/* Critical Error Information  */
		    return (_LO(ax) != 0x06 && _LO(ax) != 0x0b);
		case 0x5e:
		    return (_LO(ax) != 0x03);
	    }
	    break;
	case 0x25:			/* Absolute Disk Read */
	case 0x26:			/* Absolute Disk write */
	    return 1;
    }

    return 0;
}

static int need_copy_eseg(struct sigcontext_struct *scp, int intr)
{
    switch (intr) {
	case 0x10:			/* video */
	    switch (_HI(ax)) {
		case 0x10:		/* Set/Get Palette Registers (EGA/VGA) */
		    switch(_LO(ax)) {
			case 0x2:		/* set all palette registers and border */
			case 0x09:		/* ead palette registers and border (PS/2) */
			case 0x12:		/* set block of DAC color registers */
			case 0x17:		/* read block of DAC color registers */
			    return 1;
		    }
		    break;
		case 0x11:		/* Character Generator Routine (EGA/VGA) */
		    switch (_LO(ax)) {
			case 0x0:		/* user character load */
			case 0x10:		/* user specified character definition table */
			case 0x20: case 0x21:
			    return 1;
		    }
		    break;
		case 0x13:		/* Write String */
		case 0x15:		/*  Return Physical Display Parms */
		case 0x1b:
		    return 1;
		case 0x1c:
		    if (_LO(ax) == 1 || _LO(ax) == 2)
			return 1;
		    break;
	}
	break;
	case 0x21:
	    switch (_HI(ax)) {
		case 0x57:		/* Get/Set File Date and Time Using Handle */
		    if ((_LO(ax) == 0) || (_LO(ax) == 1)) {
			return 0;
		    }
		    return 1;
		case 0x5e:
		    return (_LO(ax) == 0x03);
	    }
	    break;
	case 0x33:
	    switch (_HI(ax)) {
		case 0x16:		/* save state */
		case 0x17:		/* restore */
		    return 1;
	    }
	    break;
    }

    return 0;
}

/* DOS selector is a selector whose base address is less than 0xffff0 */
/* and para. aligned.                                                 */
static int in_dos_space(unsigned short sel, unsigned long off)
{
    unsigned long base = Segments[sel >> 3].base_addr;

    if (base + off > 0x10ffef) {	/* ffff:ffff for DOS high */
      D_printf("MSDOS: base address %#lx of sel %#x > DOS limit\n", base, sel);
      return 0;
    } else
    if (base & 0xf) {
      D_printf("MSDOS: base address %#lx of sel %#x not para. aligned.\n", base, sel);
      return 0;
    } else
      return 1;
}

static void old_dos_terminate(struct sigcontext_struct *scp, int i)
{
    unsigned short psp_seg_sel, parent_psp = 0;
    unsigned short psp_sig;

    D_printf("MSDOS: old_dos_terminate, int=%#x\n", i);

    REG(cs)  = MSDOS_CLIENT.current_psp;
    REG(eip) = 0x100;

#if 0
    _eip = READ_WORD(SEGOFF2LINEAR(MSDOS_CLIENT.current_psp, 0xa));
    _cs = ConvertSegmentToCodeDescriptor(
      READ_WORD(SEGOFF2LINEAR(MSDOS_CLIENT.current_psp, 0xa+2)));
#endif

    /* put our return address there */
    WRITE_WORD(SEGOFF2LINEAR(MSDOS_CLIENT.current_psp, 0xa),
	     DPMI_OFF + HLT_OFF(DPMI_return_from_dosint) + i);
    WRITE_WORD(SEGOFF2LINEAR(MSDOS_CLIENT.current_psp, 0xa+2), DPMI_SEG);

    psp_seg_sel = READ_WORD(SEGOFF2LINEAR(MSDOS_CLIENT.current_psp, 0x16));
    /* try segment */
    psp_sig = READ_WORD(SEG2LINEAR(psp_seg_sel));
    if (psp_sig != 0x20CD) {
	/* now try selector */
	unsigned long addr;
	D_printf("MSDOS: Trying PSP sel=%#x, V=%i, d=%i, l=%#lx\n",
	    psp_seg_sel, ValidAndUsedSelector(psp_seg_sel),
	    in_dos_space(psp_seg_sel, 0), GetSegmentLimit(psp_seg_sel));
	if (ValidAndUsedSelector(psp_seg_sel) && in_dos_space(psp_seg_sel, 0) &&
		GetSegmentLimit(psp_seg_sel) >= 0xff) {
	    addr = GetSegmentBaseAddress(psp_seg_sel);
	    psp_sig = READ_WORD(addr);
	    D_printf("MSDOS: Trying PSP sel=%#x, addr=%#lx\n", psp_seg_sel, addr);
	    if (!(addr & 0x0f) && psp_sig == 0x20CD) {
		/* found selector */
		parent_psp = addr >> 4;
	        D_printf("MSDOS: parent PSP sel=%#x, seg=%#x\n",
		    psp_seg_sel, parent_psp);
	    }
	}
    } else {
	/* found segment */
	parent_psp = psp_seg_sel;
    }
    if (!parent_psp) {
	/* no PSP found, use current as the last resort */
	D_printf("MSDOS: using current PSP as parent!\n");
	parent_psp = MSDOS_CLIENT.current_psp;
    }

    D_printf("MSDOS: parent PSP seg=%#x\n", parent_psp);
    if (parent_psp != psp_seg_sel)
	WRITE_WORD(SEGOFF2LINEAR(MSDOS_CLIENT.current_psp, 0x16), parent_psp);
    /* And update our PSP pointer */
    MSDOS_CLIENT.current_psp = parent_psp;
}

/*
 * DANG_BEGIN_FUNCTION msdos_pre_extender
 *
 * This function is called before a protected mode client goes to real
 * mode for DOS service. All protected mode selector is changed to
 * real mode segment register. And if client\'s data buffer is above 1MB,
 * necessary buffer copying is performed. This function returns 1 if
 * it does not need to go to real mode, otherwise returns 0.
 *
 * DANG_END_FUNCTION
 */

int msdos_pre_extender(struct sigcontext_struct *scp, int intr)
{
    D_printf("MSDOS: pre_extender: int 0x%x, ax=0x%x\n", intr, _LWORD(eax));
    if (MSDOS_CLIENT.user_dta_sel && intr == 0x21) {
	switch (_HI(ax)) {	/* functions use DTA */
	case 0x11: case 0x12:	/* find first/next using FCB */
	case 0x4e: case 0x4f:	/* find first/next */
	    MEMCPY_DOS2DOS(DTA_under_1MB, DTA_over_1MB, 0x80);
	    break;
	}
    }

    /* only consider DOS and some BIOS services */
    switch (intr) {
    case 0x41:			/* win debug */
	return MSDOS_DONE;

    case 0x15:			/* misc */
	switch (_HI(ax)) {
	  case 0xc2:
	    D_printf("MSDOS: PS2MOUSE function 0x%x\n", _LO(ax));
	    switch (_LO(ax)) {
	      case 0x07:		/* set handler addr */
		if ( _es && D_16_32(_ebx) ) {
		  D_printf("MSDOS: PS2MOUSE: set handler addr 0x%x:0x%lx\n",
		    _es, D_16_32(_ebx));
		  MSDOS_CLIENT.PS2mouseCallBack.selector = _es;
		  MSDOS_CLIENT.PS2mouseCallBack.offset = D_16_32(_ebx); 
		  REG(es) = DPMI_SEG;
		  REG(ebx) = DPMI_OFF + HLT_OFF(MSDOS_PS2_mouse_callback);
		} else {
		  D_printf("MSDOS: PS2MOUSE: reset handler addr\n");
		  REG(es) = 0;
		  REG(ebx) = 0;
		}
		return 0;
	      default:
		return 0;
	    }
	    break;
	  default:
	    return 0;
	}
    case 0x20:			/* DOS terminate */
	old_dos_terminate(scp, intr);
	return 0;
    case 0x21:
	switch (_HI(ax)) {
	    /* first see if we don\'t need to go to real mode */
	case 0x25: {		/* set vector */
	      INTDESC desc;
	      desc.selector = _ds;
	      desc.offset = D_16_32(_edx);
	      dpmi_set_interrupt_vector(_LO(ax), desc);
	      D_printf("MSDOS: int 21,ax=0x%04x, ds=0x%04x. dx=0x%04x\n",
		     _LWORD(eax), _ds, _LWORD(edx));
	    }
	    return MSDOS_DONE;
	case 0x35: {	/* Get Interrupt Vector */
	      INTDESC desc = dpmi_get_interrupt_vector(_LO(ax));
	      _es = desc.selector;
	      _ebx = desc.offset;
	      D_printf("MSDOS: int 21,ax=0x%04x, es=0x%04x. bx=0x%04x\n",
		     _LWORD(eax), _es, _LWORD(ebx));
	    }
	    return MSDOS_DONE;
	case 0x48:		/* allocate memory */
	    {
		dpmi_pm_block *bp = DPMImalloc(_LWORD(ebx)<<4);
		if (!bp) {
		    _eflags |= CF;
		    _LWORD(ebx) = dpmi_free_memory >> 4;
		    _LWORD(eax) = 0x08;
		} else {
		    unsigned short sel = AllocateDescriptors(1);
		    SetSegmentBaseAddress(sel, (unsigned long)bp->base);
		    SetSegmentLimit(sel, bp -> size - 1);
		    _LWORD(eax) = sel;
		    _eflags &= ~CF;
		}
		return MSDOS_DONE;
	    }
	case 0x49:		/* free memory */
	    {
		unsigned long h =
		    base2handle((void *)GetSegmentBaseAddress(_es));
		if (!h) 
		    _eflags |= CF;
		else {
		    _eflags &= ~CF;
		    DPMIfree(h);
		    FreeDescriptor(_es);
		    FreeSegRegs(scp, _es);
		}
		return MSDOS_DONE;
	    }
	case 0x4a:		/* reallocate memory */
	    {
		unsigned long h;
		dpmi_pm_block *bp;

		h = base2handle((void *)GetSegmentBaseAddress(_es));
		if (!h) {
		    _eflags |= CF;
		    return MSDOS_DONE;
		}
		bp = DPMIrealloc(h, _LWORD(ebx)<<4);
		if (!bp) {
		    _eflags |= CF;
		    _LWORD(ebx) = dpmi_free_memory >> 4;
		    _LWORD(eax) = 0x08;
		} else {
		    SetSegmentBaseAddress(_es, (unsigned long)bp->base);
		    SetSegmentLimit(_es, bp -> size - 1);
		    _eflags &= ~CF;
		}
		return MSDOS_DONE;
	    }
	case 0x01 ... 0x08:	/* These are dos functions which */
	case 0x0b ... 0x0e:	/* are not required memory copy, */
	case 0x19:		/* and segment register translation. */
	case 0x2a ... 0x2e:
	case 0x30 ... 0x34:
	case 0x36: case 0x37:
	case 0x3e:
	case 0x42:
	case 0x45: case 0x46:
	case 0x4d:
	case 0x4f:		/* find next */
	case 0x54:
	case 0x58: case 0x59:
	case 0x5c:		/* lock */
	case 0x66 ... 0x68:	
	case 0xF8:		/* OEM SET vector */
	    return 0;
	case 0x00:		/* DOS terminate */
	    old_dos_terminate(scp, intr);
	    LWORD(eax) = 0x4c00;
	    return 0;
	case 0x09:		/* Print String */
	    {
		int i;
		char *s, *d;
		prepare_ems_frame();
		REG(ds) = TRANS_BUFFER_SEG;
		REG(edx) = 0;
		d = (char *)(REG(ds)<<4);
		s = (char *)GetSegmentBaseAddress(_ds) + D_16_32(_edx);
		for(i=0; i<0xffff; i++, d++, s++) {
		    *d = *s;
		    if( *s == '$')
			break;
		}
	    }
	    return 0;
	case 0x1a:		/* set DTA */
	  {
	    unsigned long off = D_16_32(_edx);
	    if ( !in_dos_space(_ds, off)) {
		MSDOS_CLIENT.user_dta_sel = _ds;
		MSDOS_CLIENT.user_dta_off = off;
		REG(ds) = MSDOS_CLIENT.lowmem_seg+DTA_Para_ADD;
		REG(edx)=0;
                MEMCPY_DOS2DOS(DTA_under_1MB, DTA_over_1MB, 0x80);
	    } else {
                REG(ds) = GetSegmentBaseAddress(_ds) >> 4;
                MSDOS_CLIENT.user_dta_sel = 0;
            }
	  }
	  return 0;
            
	/* FCB functions */	    
	case 0x0f: case 0x10:	/* These are not supported by */
	case 0x14: case 0x15:	/* dosx.exe, according to Ralf Brown */
	case 0x21 ... 0x24:
	case 0x27: case 0x28:
	    error("MS-DOS: Unsupported function 0x%x\n", _HI(ax));
	    _HI(ax) = 0xff;
	    return MSDOS_DONE;
	case 0x11: case 0x12:	/* find first/next using FCB */
	case 0x13:		/* Delete using FCB */
	case 0x16:		/* Create usring FCB */
	case 0x17:		/* rename using FCB */
	    prepare_ems_frame();
	    REG(ds) = TRANS_BUFFER_SEG;
	    REG(edx)=0;
	    MEMCPY_DOS2DOS(SEG_ADR((void *), ds, dx),
			(void *)GetSegmentBaseAddress(_ds) + D_16_32(_edx),
			0x50);
	    return 0;
	case 0x29:		/* Parse a file name for FCB */
	    {
		unsigned short seg = TRANS_BUFFER_SEG;
		prepare_ems_frame();
		REG(ds) = seg;
		REG(esi) = 0;
		MEMCPY_DOS2DOS(SEG_ADR((void *), ds, si),
			    (void *)GetSegmentBaseAddress(_ds) + D_16_32(_esi),
			    0x100);
		seg += 0x10;
		REG(es) = seg;
		REG(edi) = 0;
		MEMCPY_DOS2DOS(SEG_ADR((void *), es, di),
			    (void *)GetSegmentBaseAddress(_es) + D_16_32(_edi),
			    0x50);
	    }
	    return 0;
	case 0x47:		/* GET CWD */
	    prepare_ems_frame();
	    REG(ds) = TRANS_BUFFER_SEG;
	    REG(esi) = 0;
	    return 0;
	case 0x4b:		/* EXEC */
	    D_printf("BCC: call dos exec.\n");
	    REG(cs) = DPMI_SEG;
	    REG(eip) = DPMI_OFF + HLT_OFF(DPMI_return_from_dos_exec);
	    msdos_pre_exec(scp);
	    return MSDOS_ALT_RET | MSDOS_NEED_FORK;

	case 0x50:		/* set PSP */
	  {
	    unsigned short envp;
	    if ( !in_dos_space(_LWORD(ebx), 0)) {
		MSDOS_CLIENT.user_psp_sel = _LWORD(ebx);
		LWORD(ebx) = MSDOS_CLIENT.current_psp;
		MEMCPY_DOS2DOS((void *)SEG2LINEAR(LWORD(ebx)), 
		    (void *)GetSegmentBaseAddress(_LWORD(ebx)), 0x100);
		D_printf("MSDOS: PSP moved from %p to %p\n",
		    (char *)GetSegmentBaseAddress(_LWORD(ebx)),
		    (void *)SEG2LINEAR(LWORD(ebx)));
	    } else {
		REG(ebx) = GetSegmentBaseAddress(_LWORD(ebx)) >> 4;
		MSDOS_CLIENT.user_psp_sel = 0;
	    }
	    MSDOS_CLIENT.current_psp = LWORD(ebx);
	    envp = *(unsigned short *)(((char *)(LWORD(ebx)<<4)) + 0x2c);
	    if (envp && !in_dos_space(envp, 0)) {
		/* DANG_FIXTHIS: Please implement the ENV translation! */
		error("FIXME: ENV translation is not implemented\n");
		MSDOS_CLIENT.current_env_sel = 0;
	    } else {
		MSDOS_CLIENT.current_env_sel = envp;
	    }
	  }
	  return 0;

	case 0x26:		/* create PSP */
	    prepare_ems_frame();
	    REG(edx) = TRANS_BUFFER_SEG;
	    return 0;

	case 0x55:		/* create & set PSP */
	    if ( !in_dos_space(_LWORD(edx), 0)) {
		MSDOS_CLIENT.user_psp_sel = _LWORD(edx);
		LWORD(edx) = MSDOS_CLIENT.current_psp;
	    } else {
		REG(edx) = GetSegmentBaseAddress(_LWORD(edx)) >> 4;
		MSDOS_CLIENT.current_psp = LWORD(edx);
		MSDOS_CLIENT.user_psp_sel = 0;
	    }
	    return 0;

	case 0x39:		/* mkdir */
	case 0x3a:		/* rmdir */
	case 0x3b:		/* chdir */
	case 0x3c:		/* creat */
	case 0x3d:		/* Dos OPEN */
	case 0x41:		/* unlink */
	case 0x43:		/* change attr */
	case 0x4e:		/* find first */
	case 0x5b:		/* Create */
	    if ((_HI(ax) == 0x4e) && (_ecx & 0x8))
		D_printf("MSDOS: MS-DOS try to find volume label\n");
	    {
		char *src, *dst;
		prepare_ems_frame();
		REG(ds) = TRANS_BUFFER_SEG;
		REG(edx) = 0;
		src = (char *)GetSegmentBaseAddress(_ds) + D_16_32(_edx);
		dst = SEG_ADR((char *), ds, dx);
		D_printf("MSDOS: passing ASCIIZ > 1MB to dos %#x\n", (int)dst); 
		D_printf("%#x: '%s'\n", (int)src, src);
                snprintf(dst, MAX_DOS_PATH, "%s", src);
	    }
	    return 0;
	case 0x38:
	    if (_LWORD(edx) != 0xffff) { /* get country info */
		prepare_ems_frame();
		REG(ds) = TRANS_BUFFER_SEG;
		REG(edx) = 0;
	    }
	    break;
	case 0x3f:		/* dos read */
	    set_io_buffer((char*)GetSegmentBaseAddress(_ds) + D_16_32(_edx),
		D_16_32(_ecx));
	    prepare_ems_frame();
	    REG(ds) = TRANS_BUFFER_SEG;
	    REG(edx) = 0;
	    REG(ecx) = D_16_32(_ecx);
	    fake_int_to(DOS_LONG_READ_SEG, DOS_LONG_READ_OFF);
	    return MSDOS_ALT_ENT;
	case 0x40:		/* DOS Write */
	    set_io_buffer((char*)GetSegmentBaseAddress(_ds) + D_16_32(_edx),
		D_16_32(_ecx));
	    prepare_ems_frame();
	    REG(ds) = TRANS_BUFFER_SEG;
	    REG(edx) = 0;
	    REG(ecx) = D_16_32(_ecx);
	    fake_int_to(DOS_LONG_WRITE_SEG, DOS_LONG_WRITE_OFF);
	    return MSDOS_ALT_ENT;
	case 0x53:		/* Generate Drive Parameter Table  */
	    {
		unsigned short seg = TRANS_BUFFER_SEG;
		prepare_ems_frame();
		REG(ds) = seg;
		REG(esi) = 0;
		MEMCPY_DOS2DOS(SEG_ADR((void *), ds, si),
			    (void *)GetSegmentBaseAddress(_ds) + D_16_32(_esi),
			    0x30);
		seg += 30;

		REG(es) = seg;
		REG(ebp) = 0;
		MEMCPY_DOS2DOS(SEG_ADR((void *), es, bp),
			    (void *)GetSegmentBaseAddress(_es) + D_16_32(_ebp),
			    0x60);
	    }
	    return 0;
	case 0x56:		/* rename file */
	    {
		unsigned short seg = TRANS_BUFFER_SEG;
		prepare_ems_frame();
		REG(ds) = seg;
		REG(edx) = 0;
		snprintf((char *)(REG(ds)<<4), MAX_DOS_PATH, "%s",
			     (char *)GetSegmentBaseAddress(_ds) + D_16_32(_edx));
		seg += 0x20;

		REG(es) = seg;
		REG(edi) = 0;
		snprintf((char *)(REG(es)<<4), MAX_DOS_PATH, "%s",
			     (char *)GetSegmentBaseAddress(_es) + D_16_32(_edi));
	    }
	    return 0;
	case 0x5f:		/* redirection */
	    switch (_LO(ax)) {
	    case 0: case 1:
		return 0;
	    case 2 ... 6:
		prepare_ems_frame();
		REG(ds) = TRANS_BUFFER_SEG;
		REG(esi) = 0;
		MEMCPY_DOS2DOS(SEG_ADR((void *), ds, si),
			(void *)GetSegmentBaseAddress(_ds) + D_16_32(_esi),
			0x100);
		REG(es) = TRANS_BUFFER_SEG + 0x10;
		REG(edi) = 0;
		MEMCPY_DOS2DOS(SEG_ADR((void *), es, di),
			(void *)GetSegmentBaseAddress(_es) + D_16_32(_edi),
			0x100);
		return 0;
	    }
	case 0x60:		/* Get Fully Qualified File Name */
	    prepare_ems_frame();
	    REG(ds) = TRANS_BUFFER_SEG;
	    REG(esi) = 0;
	    MEMCPY_DOS2DOS(SEG_ADR((void *), ds, si),
		    (void *)GetSegmentBaseAddress(_ds) + D_16_32(_esi),
		    0x100);
	    REG(es) = TRANS_BUFFER_SEG + 0x10;
	    REG(edi) = 0;
	    return 0;
	case 0x6c:		/*  Extended Open/Create */
	    {
		char *src, *dst;
		prepare_ems_frame();
		REG(ds) = TRANS_BUFFER_SEG;
		REG(esi) = 0;
		src = (char *)GetSegmentBaseAddress(_ds) + D_16_32(_esi);
		dst = SEG_ADR((char *), ds, si);
		D_printf("MSDOS: passing ASCIIZ > 1MB to dos %#x\n", (int)dst); 
		D_printf("%#x: '%s'\n", (int)src, src);
		snprintf(dst, MAX_DOS_PATH, "%s", src);
	    }
	    return 0;
	case 0x65:		/* internationalization */
    	    switch (_LO(ax)) {
		case 0:
		    prepare_ems_frame();
		    REG(es) = TRANS_BUFFER_SEG;
		    REG(edi) = 0;
		    MEMCPY_DOS2DOS(SEG_ADR((void *), es, di),
			(void *)GetSegmentBaseAddress(_es) + D_16_32(_edi),
			_LWORD(ecx));
		    break;
		case 1 ... 7:
		    prepare_ems_frame();
		    REG(es) = TRANS_BUFFER_SEG;
		    REG(edi) = 0;
		    break;
		case 0x21:
		case 0xa1:
		    prepare_ems_frame();
		    REG(ds) = TRANS_BUFFER_SEG;
		    REG(edx) = 0;
		    MEMCPY_DOS2DOS(SEG_ADR((void *), ds, dx),
			(void *)GetSegmentBaseAddress(_ds) + D_16_32(_edx),
			_LWORD(ecx));
		    break;
		case 0x22:
		case 0xa2:
		    prepare_ems_frame();
		    REG(ds) = TRANS_BUFFER_SEG;
		    REG(edx) = 0;
		    strcpy(SEG_ADR((void *), ds, dx),
			(void *)GetSegmentBaseAddress(_ds) + D_16_32(_edx));
		    break;
	    }
            return 0;
    case 0x71:     /* LFN functions */
        {
        char *src, *dst;
        switch (_LO(ax)) {
        case 0x3B: /* change dir */
        case 0x41: /* delete file */
        case 0x43: /* get file attributes */
            REG(ds) = TRANS_BUFFER_SEG;
            REG(edx) = 0;
            src = (char *)GetSegmentBaseAddress(_ds) + D_16_32(_edx);
            dst = SEG_ADR((char *), ds, dx);
            snprintf(dst, MAX_DOS_PATH, "%s", src);
            return 0;
        case 0x4E: /* find first file */
            REG(ds) = TRANS_BUFFER_SEG;
            REG(edx) = 0;
            REG(es) = TRANS_BUFFER_SEG;
            REG(edi) = MAX_DOS_PATH;
            src = (char *)GetSegmentBaseAddress(_ds) + D_16_32(_edx);
            dst = SEG_ADR((char *), ds, dx);
            snprintf(dst, MAX_DOS_PATH, "%s", src);
            return 0;
        case 0x4F: /* find next file */
            REG(es) = TRANS_BUFFER_SEG;
            REG(edi) = 0;
            src = (char *)GetSegmentBaseAddress(_es) + D_16_32(_edi);
            dst = SEG_ADR((char *), es, di);
            MEMCPY_DOS2DOS(dst, src, 0x13e);
            return 0;
        case 0x47: /* get cur dir */
            REG(ds) = TRANS_BUFFER_SEG;
            REG(esi) = 0;
            return 0;
	case 0x60: /* canonicalize filename */
	    REG(ds) = TRANS_BUFFER_SEG;
	    REG(esi) = 0;
	    REG(es) = TRANS_BUFFER_SEG;
	    REG(edi) = MAX_DOS_PATH;
	    src = (char *)GetSegmentBaseAddress(_ds) + D_16_32(_esi);	
	    dst = SEG_ADR((char *), ds, si);
	    snprintf(dst, MAX_DOS_PATH, "%s", src);
	    return 0;
        case 0x6c: /* extended open/create */
            REG(ds) = TRANS_BUFFER_SEG;
            REG(esi) = 0;
            src = (char *)GetSegmentBaseAddress(_ds) + D_16_32(_esi);
            dst = SEG_ADR((char *), ds, si);
            snprintf(dst, MAX_DOS_PATH, "%s", src);
            return 0;
        case 0xA0: /* get volume info */
            REG(ds) = TRANS_BUFFER_SEG;
            REG(edx) = 0;
            REG(es) = TRANS_BUFFER_SEG;
            REG(edi) = MAX_DOS_PATH;
            src = (char *)GetSegmentBaseAddress(_ds) + D_16_32(_edx);
            dst = SEG_ADR((char *), ds, dx);
            snprintf(dst, MAX_DOS_PATH, "%s", src);
            return 0;
        case 0xA1: /* close find */
            return 0;
        default: /* all other subfuntions currently not supported */
            _eflags |= CF;
            _eax = _eax & 0xFFFFFF00;
            return 1;
        }
        }
	default:
	    break;
	}
	break;
    case 0x33:			/* mouse */
	switch (_LWORD(eax)) {
	case 0x09:		/* Set Mouse Graphics Cursor */
	    prepare_ems_frame();
	    REG(es) = TRANS_BUFFER_SEG;
	    REG(edx) = 0;
	    MEMCPY_DOS2DOS(SEG_ADR((void *), es, dx),
		    (void *)GetSegmentBaseAddress(_es) + D_16_32(_edx),
		    16);
	    return 0;
	case 0x0c:		/* set call back */
	case 0x14: {		/* swap call back */
	    struct pmaddr_s old_callback = MSDOS_CLIENT.mouseCallBack;
	    MSDOS_CLIENT.mouseCallBack.selector = _es;
	    MSDOS_CLIENT.mouseCallBack.offset = D_16_32(_edx);
	    if (_es) {
		D_printf("MSDOS: set mouse callback\n");
		REG(es) = DPMI_SEG;
		REG(edx) = DPMI_OFF + HLT_OFF(MSDOS_mouse_callback);
	    } else {
		D_printf("MSDOS: reset mouse callback\n");
		REG(es) = 0;
		REG(edx) = 0;
	    }
	    if (_LWORD(eax) == 0x14) {
		_es = old_callback.selector;
		if (MSDOS_CLIENT.is_32)
		    _edx = old_callback.offset;
		else
		    _LWORD(edx) = old_callback.offset;
	    }
	  }
	  return 0;
	}
	break;
    }

    if (need_copy_dseg(scp, intr)) {
	char *src, *dst;
	int len;
	prepare_ems_frame();
	REG(ds) = TRANS_BUFFER_SEG;
	src = (char *)GetSegmentBaseAddress(_ds);
	dst = (char *)(REG(ds)<<4);
	len = ((Segments[_ds >> 3].limit > 0xffff) ||
	    	Segments[_ds >> 3].is_big) ?
		0xffff : Segments[_ds >> 3].limit;
	D_printf("MSDOS: whole segment of DS at %#x copy to DOS at %#x for %#x\n",
		(int)src, (int)dst, len);
	MEMCPY_DOS2DOS(dst, src, len);
    }

    if (need_copy_eseg(scp, intr)) {
	char *src, *dst;
	int len;
	prepare_ems_frame();
	REG(es) = TRANS_BUFFER_SEG;
	src = (char *)GetSegmentBaseAddress(_es);
	dst = (char *)(REG(es)<<4);
	len = ((Segments[_es >> 3].limit > 0xffff) ||
	    	Segments[_es >> 3].is_big) ?
		0xffff : Segments[_es >> 3].limit;
	D_printf("MSDOS: whole segment of ES at %#x copy to DOS at %#x for %#x\n",
		(int)src, (int)dst, len);
	MEMCPY_DOS2DOS(dst, src, len);
    }
    return 0;
}

void msdos_pre_exec(struct sigcontext_struct *scp)
{
    /* we must copy all data from higher 1MB to lower 1MB */
    unsigned short segment = TRANS_BUFFER_SEG;
    char *p;
    unsigned short sel,off;

    /* must copy command line */
    prepare_ems_frame();
    REG(ds) = segment;
    REG(edx) = 0;
    p = (char *)GetSegmentBaseAddress(_ds) + D_16_32(_edx);
    snprintf((char *)SEG2LINEAR(REG(ds)), MAX_DOS_PATH, "%s", p);
    segment += (MAX_DOS_PATH + 0x0f) >> 4;

    /* must copy parameter block */
    REG(es) = segment;
    REG(ebx) = 0;
    MEMCPY_DOS2DOS(SEG_ADR((void *), es, bx),
       (void *)GetSegmentBaseAddress(_es) + D_16_32(_ebx), 0x20);
    segment += 2;
#if 0
    /* now the envrionment segment */
    sel = READ_WORD(SEG_ADR((unsigned short *), es, bx));
    WRITE_WORD(SEG_ADR((unsigned short *), es, bx), segment);
    MEMCPY_DOS2DOS((void *)SEG2LINEAR(segment),           /* 4K envr. */
	(void *)GetSegmentBaseAddress(sel),
	0x1000);
    segment += 0x100;
#else
    WRITE_WORD(SEG_ADR((unsigned short *), es, bx), 0);
#endif
    /* now the tail of the command line */
    off = READ_WORD(SEGOFF2LINEAR(REG(es), LWORD(ebx)+2));
    sel = READ_WORD(SEGOFF2LINEAR(REG(es), LWORD(ebx)+4));
    WRITE_WORD(SEGOFF2LINEAR(REG(es), LWORD(ebx)+4), segment);
    WRITE_WORD(SEGOFF2LINEAR(REG(es), LWORD(ebx)+2), 0);
    MEMCPY_DOS2DOS((void *)SEG2LINEAR(segment),
	   (void *)GetSegmentBaseAddress(sel) + off,
	   0x80);
    segment += 8;

    /* set the FCB pointers to something reasonable */
    WRITE_WORD(SEGOFF2LINEAR(REG(es), LWORD(ebx)+6), 0);
    WRITE_WORD(SEGOFF2LINEAR(REG(es), LWORD(ebx)+8), segment);
    WRITE_WORD(SEGOFF2LINEAR(REG(es), LWORD(ebx)+0xA), 0);
    WRITE_WORD(SEGOFF2LINEAR(REG(es), LWORD(ebx)+0xC), segment);
    memset((void *)SEG2LINEAR(segment), 0, 0x30);
    segment += 3;

    /* then the enviroment seg */
    if (MSDOS_CLIENT.current_env_sel)
	WRITE_WORD(SEGOFF2LINEAR(MSDOS_CLIENT.current_psp, 0x2c),
	    GetSegmentBaseAddress(MSDOS_CLIENT.current_env_sel) >> 4);
}

void msdos_post_exec(struct sigcontext_struct *scp)
{
    _eflags = 0x0202 | (0x0dd5 & REG(eflags));
    _eax = REG(eax);
    if (!(LWORD(eflags) & CF)) {
        _ebx = REG(ebx);
        _edx = REG(edx);
     }

    if (MSDOS_CLIENT.current_env_sel)
	WRITE_WORD(SEGOFF2LINEAR(MSDOS_CLIENT.current_psp, 0x2c),
	    MSDOS_CLIENT.current_env_sel);

    restore_ems_frame();
}

/*
 * DANG_BEGIN_FUNCTION msdos_post_extender
 *
 * This function is called after return from real mode DOS service
 * All real mode segment registers are changed to protected mode selectors
 * And if client\'s data buffer is above 1MB, necessary buffer copying
 * is performed.
 *
 * DANG_END_FUNCTION
 */

int msdos_post_extender(struct sigcontext_struct *scp, int intr)
{
    int update_mask = ~0;
#define PRESERVE1(rg) (update_mask &= ~(1 << rg##_INDEX))
#define PRESERVE2(rg1, rg2) (update_mask &= ~((1 << rg1##_INDEX) | (1 << rg2##_INDEX)))
#define SET_REG(rg, val) (PRESERVE1(rg), _##rg = (val))
    D_printf("MSDOS: post_extender: int 0x%x ax=0x%04x\n", intr, _LWORD(eax));

    if (MSDOS_CLIENT.user_dta_sel && intr == 0x21 ) {
	switch (_HI(ax)) {	/* functions use DTA */
	case 0x11: case 0x12:	/* find first/next using FCB */
	case 0x4e: case 0x4f:	/* find first/next */
	    MEMCPY_DOS2DOS(DTA_over_1MB, DTA_under_1MB, 0x80);
	    break;
	}
    }

    if (need_copy_dseg(scp, intr)) {
	unsigned short my_ds;
	char *src, *dst;
	int len;
	my_ds = TRANS_BUFFER_SEG;
	src = (char *)(my_ds<<4);
	dst = (char *)GetSegmentBaseAddress(_ds);
	len = ((Segments[_ds >> 3].limit > 0xffff) ||
	    	Segments[_ds >> 3].is_big) ?
		0xffff : Segments[_ds >> 3].limit;
	D_printf("MSDOS: DS seg at %#x copy back at %#x for %#x\n",
		(int)src, (int)dst, len);
	MEMCPY_DOS2DOS(dst, src, len);
    } 

    if (need_copy_eseg(scp, intr)) {
	unsigned short my_es;
	char *src, *dst;
	int len;
	my_es = TRANS_BUFFER_SEG;
	src = (char *)(my_es<<4);
	dst = (char *)GetSegmentBaseAddress(_es);
	len = ((Segments[_es >> 3].limit > 0xffff) ||
	    	Segments[_es >> 3].is_big) ?
		0xffff : Segments[_es >> 3].limit;
	D_printf("MSDOS: ES seg at %#x copy back at %#x for %#x\n",
		(int)src, (int)dst, len);
	MEMCPY_DOS2DOS(dst, src, len);
    } 

    switch (intr) {
    case 0x10:			/* video */
	if (_LWORD(eax) == 0x1130) {
	    /* get current character generator infor */
	    SET_REG(es,	ConvertSegmentToDescriptor(REG(es)));
	}
	break;
    case 0x15:
	/* we need to save regs at int15 because AH has the return value */
	if (_HI(ax) == 0xc0) { /* Get Configuration */
                if (REG(eflags)&CF)
                        break;
                SET_REG(es, ConvertSegmentToDescriptor(REG(es)));
        }
	break;
    case 0x2f:
	switch (_LWORD(eax)) {
	    case 0x4310:
                MSDOS_CLIENT.XMS_call = MK_FARt(REG(es), LWORD(ebx));
                SET_REG(es, ConvertSegmentToCodeDescriptor(DPMI_SEG));
                SET_REG(ebx, DPMI_OFF + HLT_OFF(MSDOS_XMS_call));
		break;
	}
	break;

    case 0x21:
	switch (_HI(ax)) {
	case 0x00:		/* psp kill */
	    PRESERVE1(eax);
	    break;
	case 0x09:		/* print String */
	case 0x1a:		/* set DTA */
	    PRESERVE1(edx);
	    break;
	case 0x11: case 0x12:	/* findfirst/next using FCB */
 	case 0x13:		/* Delete using FCB */
 	case 0x16:		/* Create usring FCB */
 	case 0x17:		/* rename using FCB */
 	    PRESERVE1(edx);
	    MEMCPY_DOS2DOS((void *)GetSegmentBaseAddress(_ds) + D_16_32(_edx),
			SEG_ADR((void *), ds, dx), 0x50);
	    break;

	case 0x29:		/* Parse a file name for FCB */
	    PRESERVE2(esi, edi);
	    MEMCPY_DOS2DOS((void *)GetSegmentBaseAddress(_ds) + D_16_32(_esi),
		/* Warning: SI altered, assume old value = 0, don't touch. */
			    (void *)(REG(ds)<<4), 0x100);
	    SET_REG(esi, _esi + LWORD(esi));
	    MEMCPY_DOS2DOS((void *)(GetSegmentBaseAddress(_es) + D_16_32(_edi)),
			    SEG_ADR((void *), es, di),  0x50);
	    break;

	case 0x2f:		/* GET DTA */
	    if (SEG_ADR((void*), es, bx) == DTA_under_1MB) {
		if (!MSDOS_CLIENT.user_dta_sel)
		    error("Selector is not set for the translated DTA\n");
		SET_REG(es, MSDOS_CLIENT.user_dta_sel);
		SET_REG(ebx, MSDOS_CLIENT.user_dta_off);
	    } else {
		SET_REG(es, ConvertSegmentToDescriptor(REG(es)));
		/* it is important to copy only the lower word of ebx
		 * and make the higher word zero, so do it here instead
		 * of relying on dpmi.c */
		SET_REG(ebx, LWORD(ebx));
	    }
	    break;

	case 0x34:		/* Get Address of InDOS Flag */
	case 0x35:		/* GET Vector */
	case 0x52:		/* Get List of List */
	    SET_REG(es, ConvertSegmentToDescriptor(REG(es)));
	    break;

	case 0x39:		/* mkdir */
	case 0x3a:		/* rmdir */
	case 0x3b:		/* chdir */
	case 0x3c:		/* creat */
	case 0x3d:		/* Dos OPEN */
	case 0x41:		/* unlink */
	case 0x43:		/* change attr */
	case 0x4e:		/* find first */
	case 0x5b:		/* Create */
	    PRESERVE1(edx);
	    break;

	case 0x50:		/* Set PSP */
	    PRESERVE1(ebx);
	    break;

	case 0x6c:		/*  Extended Open/Create */
	    PRESERVE1(esi);
	    break;
	    
	case 0x55:		/* create & set PSP */
	  {
	    unsigned short envp;
	    PRESERVE1(edx);
	    envp = READ_WORD(SEGOFF2LINEAR(LWORD(edx), 0x2c));
	    MSDOS_CLIENT.current_env_sel = ConvertSegmentToDescriptor(envp);
	    if ( !in_dos_space(_LWORD(edx), 0)) {
		MEMCPY_DOS2DOS((void *)GetSegmentBaseAddress(_LWORD(edx)),
		    SEG2LINEAR(LWORD(edx)), 0x100);
	    }
	  }
	  break;

	case 0x26:		/* create PSP */
	    PRESERVE1(edx);
	    MEMCPY_DOS2DOS((void *)GetSegmentBaseAddress(_LWORD(edx)),
		(void *)SEG2LINEAR(LWORD(edx)), 0x100);
	  break;

        case 0x59:		/* Get EXTENDED ERROR INFORMATION */
	    if(LWORD(eax) == 0x22) { /* only this code has a pointer */
		SET_REG(es, ConvertSegmentToDescriptor(REG(es)));
	    }
	    break;
	case 0x38:
	    if (_LWORD(edx) != 0xffff) { /* get country info */
		PRESERVE1(edx);
		if (LWORD(eflags) & CF)
		    break;
		/* FreeDOS copies only 0x18 bytes */
		MEMCPY_DOS2DOS((void *)(GetSegmentBaseAddress(_ds) +
		    D_16_32(_edx)), SEG_ADR((void *), ds, dx), 0x18);
	    }
	    break;
	case 0x47:		/* get CWD */
	    PRESERVE1(esi);
	    if (LWORD(eflags) & CF)
		break;
	    snprintf((char *)(GetSegmentBaseAddress(_ds) +
			D_16_32(_esi)), 0x40, "%s", 
		        SEG_ADR((char *), ds, si));
	    D_printf("MSDOS: CWD: %s\n",(char *)(GetSegmentBaseAddress(_ds) +
			D_16_32(_esi)));
	    break;
#if 0	    
	case 0x48:		/* allocate memory */
	    if (LWORD(eflags) & CF)
		break;
	    SET_REG(eax, ConvertSegmentToDescriptor(LWORD(eax)));
	    break;
#endif	    
	case 0x51:		/* get PSP */
	case 0x62:
	    {/* convert environment pointer to a descriptor*/
		unsigned short 
#if 0
		envp,
#endif
		psp;
		psp = LWORD(ebx);
#if 0
		envp = *(unsigned short *)(((char *)(psp<<4))+0x2c);
		envp = ConvertSegmentToDescriptor(envp);
		*(unsigned short *)(((char *)(psp<<4))+0x2c) = envp;
#endif
		if (psp == MSDOS_CLIENT.current_psp && MSDOS_CLIENT.user_psp_sel) {
		    SET_REG(ebx, MSDOS_CLIENT.user_psp_sel);
		} else {
		    SET_REG(ebx, ConvertSegmentToDescriptor_lim(psp, 0xff));
		}
	    }
	    break;
	case 0x53:		/* Generate Drive Parameter Table  */
	    PRESERVE2(esi, ebp);
	    MEMCPY_DOS2DOS((void *)GetSegmentBaseAddress(_es) + D_16_32(_ebp),
			    SEG_ADR((void *), es, bp),
			    0x60);
	    break ;
	case 0x56:		/* rename */
	    PRESERVE2(edx, edi);
	    break ;
	case 0x5d:
	    if (_LO(ax) == 0x06 || _LO(ax) == 0x0b) /* get address of DOS swappable area */
				/*        -> DS:SI                     */
		SET_REG(ds, ConvertSegmentToDescriptor(REG(ds)));
	    break;
	case 0x3f:
	    unset_io_buffer();
	    PRESERVE2(edx, ecx);
	    break;
	case 0x40:
	    unset_io_buffer();
	    PRESERVE2(edx, ecx);
	    break;
	case 0x5f:		/* redirection */
	    switch (_LO(ax)) {
	    case 0: case 1:
		break ;
	    case 2 ... 6:
		PRESERVE2(esi, edi);
		MEMCPY_DOS2DOS((void *)GetSegmentBaseAddress(_ds)
			+ D_16_32(_esi),
			SEG_ADR((void *), ds, si),
			0x100);
		MEMCPY_DOS2DOS((void *)GetSegmentBaseAddress(_es)
			+ D_16_32(_edi),
			SEG_ADR((void *), es, di),
			0x100);
	    }
	    break;
	case 0x60:		/* Canonicalize file name */
	    PRESERVE2(esi, edi);
	    MEMCPY_DOS2DOS((void *)GetSegmentBaseAddress(_es)
			+ D_16_32(_edi),
			SEG_ADR((void *), es, di),
			0x80);
	    break;
	case 0x65:		/* internationalization */
	    PRESERVE2(edi, edx);
	    if (LWORD(eflags) & CF)
		break;
    	    switch (_LO(ax)) {
		case 1 ... 7:
		    MEMCPY_DOS2DOS((void *)GetSegmentBaseAddress(_es)
			+ D_16_32(_edi),
			SEG_ADR((void *), es, di),
			LWORD(ecx));
		    break;
		case 0x21:
		case 0xa1:
		    MEMCPY_DOS2DOS((void *)GetSegmentBaseAddress(_ds)
			+ D_16_32(_edx),
			SEG_ADR((void *), ds, dx),
			_LWORD(ecx));
		    break;
		case 0x22:
		case 0xa2:
		    strcpy((void *)GetSegmentBaseAddress(_ds)
			+ D_16_32(_edx),
			SEG_ADR((void *), ds, dx));
		    break;
	    }
	    break;
	case 0x71:		/* LFN functions */
        switch (_LO(ax)) {
        case 0x3B:
        case 0x41:
        case 0x43:
	    PRESERVE1(edx);
            break;
        case 0x4E:
	    PRESERVE1(edx);
            /* fall thru */
        case 0x4F:
	    PRESERVE1(edi);
            if (LWORD(eflags) & CF)
                break;
            MEMCPY_DOS2DOS((void *)GetSegmentBaseAddress(_es)
                     + D_16_32(_edi),
                     SEG_ADR((void *), es, di),
                        0x13E);
            break;
        case 0x47:
	    PRESERVE1(esi);
            if (LWORD(eflags) & CF)
                break;
	    snprintf((char *)(GetSegmentBaseAddress(_ds) +
			D_16_32(_esi)), MAX_DOS_PATH, "%s", 
		        SEG_ADR((char *), ds, si));
            break;
	case 0x60:
	    PRESERVE2(esi, edi);
	    if (LWORD(eflags) & CF)
		break;
	    snprintf((void *)GetSegmentBaseAddress(_es) + 
		D_16_32(_edi), MAX_DOS_PATH, "%s",
		SEG_ADR((char *), es, di));
	    break;
        case 0x6c:
	    PRESERVE1(esi);
            break;
        case 0xA0:
            PRESERVE2(edx, edi);
            if (LWORD(eflags) & CF)
                break;
            snprintf((void *)GetSegmentBaseAddress(_es) +
                     D_16_32(_edi), _LWORD(ecx), "%s",
                     SEG_ADR((char *), es, di));
            break;
        };

	default:
	    break;
	}
	break;
    case 0x25:			/* Absolute Disk Read */
    case 0x26:			/* Absolute Disk Write */
	/* the flags should be pushed to stack */
	if (MSDOS_CLIENT.is_32) {
	    _esp -= 4;
	    *(unsigned long *)(GetSegmentBaseAddress(_ss) + _esp - 4) =
	      REG(eflags);
	} else {
	    _esp -= 2;
	    *(unsigned short *)(GetSegmentBaseAddress(_ss) +
	      _LWORD(esp) - 2) = LWORD(eflags);
	}
	break;
    case 0x33:			/* mouse */
	switch (_LWORD(eax)) {
	case 0x09:		/* Set Mouse Graphics Cursor */
	case 0x14:		/* swap call back */
	    PRESERVE1(edx);
	    break;
	case 0x19:		/* Get User Alternate Interrupt Address */
	    SET_REG(ebx, ConvertSegmentToDescriptor(LWORD(ebx)));
	    break;
	default:
	    break;
	}
	break;
    }
    restore_ems_frame();
    return update_mask;
}

int msdos_pre_rm(struct sigcontext_struct *scp)
{
  unsigned char *lina = SEG_ADR((unsigned char *), cs, ip) - 1;
  unsigned short *ssp = (us *) (GetSegmentBaseAddress(_ss) + D_16_32(_esp));

  if (lina == (unsigned char *)(DPMI_ADD + HLT_OFF(MSDOS_mouse_callback))) {
    if (!Segments[MSDOS_CLIENT.mouseCallBack.selector >> 3].used) {
      D_printf("MSDOS: ERROR: mouse callback to unused segment\n");
      return 0;
    }
    D_printf("MSDOS: starting mouse callback\n");
    rm_to_pm_regs(scp, ~0);
    _ds = ConvertSegmentToDescriptor(REG(ds));
    _cs = MSDOS_CLIENT.mouseCallBack.selector;
    _eip = MSDOS_CLIENT.mouseCallBack.offset;

    if (MSDOS_CLIENT.is_32) {
	*--ssp = (us) 0;
	*--ssp = dpmi_sel(); 
	ssp -= 2, *((unsigned long *) ssp) =
	     DPMI_OFF + HLT_OFF(MSDOS_return_from_pm);
	_esp -= 8;
    } else {
	*--ssp = dpmi_sel(); 
	*--ssp = DPMI_OFF + HLT_OFF(MSDOS_return_from_pm);
	_LWORD(esp) -= 4;
    }

  } else if (lina ==(unsigned char *)(DPMI_ADD +
				      HLT_OFF(MSDOS_PS2_mouse_callback))) {
    unsigned short *rm_ssp;
    if (!Segments[MSDOS_CLIENT.PS2mouseCallBack.selector >> 3].used) {
      D_printf("MSDOS: ERROR: PS2 mouse callback to unused segment\n");
      return 0;
    }
    D_printf("MSDOS: starting PS2 mouse callback\n");

    _cs = MSDOS_CLIENT.PS2mouseCallBack.selector;
    _eip = MSDOS_CLIENT.PS2mouseCallBack.offset;

    rm_ssp = (unsigned short *)SEGOFF2LINEAR(LWORD(ss), LWORD(esp) + 4 + 8);

    if (MSDOS_CLIENT.is_32) {
	*--ssp = (us) 0;
	*--ssp = *--rm_ssp;
	D_printf("data: 0x%x ", *ssp);
	*--ssp = (us) 0;
	*--ssp = *--rm_ssp;
	D_printf("0x%x ", *ssp);
	*--ssp = (us) 0;
	*--ssp = *--rm_ssp;
	D_printf("0x%x ", *ssp);
	*--ssp = (us) 0;
	*--ssp = *--rm_ssp;
	D_printf("0x%x\n", *ssp);
	*--ssp = (us) 0;
	*--ssp = dpmi_sel(); 
	ssp -= 2, *((unsigned long *) ssp) =
	     DPMI_OFF + HLT_OFF(MSDOS_return_from_pm);
	_esp -= 24;
    } else {
	*--ssp = *--rm_ssp;
	D_printf("data: 0x%x ", *ssp);
	*--ssp = *--rm_ssp;
	D_printf("0x%x ", *ssp);
	*--ssp = *--rm_ssp;
	D_printf("0x%x ", *ssp);
	*--ssp = *--rm_ssp;
	D_printf("0x%x\n", *ssp);
	*--ssp = dpmi_sel(); 
	*--ssp = DPMI_OFF + HLT_OFF(MSDOS_return_from_pm);
	_LWORD(esp) -= 12;
    }
  }
  return 1;
}

void msdos_post_rm(struct sigcontext_struct *scp)
{
  pm_to_rm_regs(scp, ~0);
}

int msdos_pre_pm(struct sigcontext_struct *scp)
{
  if (_eip==DPMI_OFF+1+HLT_OFF(MSDOS_XMS_call)) {
    D_printf("MSDOS: XMS call to 0x%x:0x%x\n",
	MSDOS_CLIENT.XMS_call.segment, MSDOS_CLIENT.XMS_call.offset);
    pm_to_rm_regs(scp, ~0);
    REG(cs) = DPMI_SEG;
    REG(eip) = DPMI_OFF + HLT_OFF(MSDOS_return_from_rm);
    fake_call_to(MSDOS_CLIENT.XMS_call.segment, MSDOS_CLIENT.XMS_call.offset);
  } else {
    error("MSDOS: unknown pm call %p\n", _eip);
    return 0;
  }
  return 1;
}

void msdos_post_pm(struct sigcontext_struct *scp)
{
  rm_to_pm_regs(scp, ~0);
}



static int
decode_pop_segreg(struct sigcontext_struct *scp, unsigned short
		       *segment, unsigned char * sreg)
{
    unsigned short *ssp;
    unsigned char *csp;
    int len;

    csp = (unsigned char *) SEL_ADR(_cs, _eip);
    ssp = (unsigned short *) SEL_ADR(_ss, _esp);
    len = 0;
    switch (*csp) {
    case 0x1f:			/* pop ds */
	len = 1;
	*segment = *ssp;
	*sreg = ds_INDEX;
	break;
    case 0x07:			/* pop es */
	len = 1;
	*segment = *ssp;
	*sreg = es_INDEX;
	break;
    case 0x17:			/* pop ss */
	len = 1;
	*segment = *ssp;
	*sreg = ss_INDEX;
	break;
    case 0x0f:		/* two byte opcode */
	csp++;
	switch (*csp) {
	case 0xa1:		/* pop fs */
	    len = 2;
	    *segment = *ssp;
	    *sreg = fs_INDEX;
	    break;
	case 0xa9:		/* pop gs */
	    len = 2;
	    *segment = *ssp;
	    *sreg = gs_INDEX;
	break;
	}
    }
    return len;
}

static int
decode_retf_iret(struct sigcontext_struct *scp, unsigned short *segment,
    unsigned char * sreg, int decode_use_16bit)
{
    unsigned short *ssp;
    unsigned char *csp;
    int len;

    csp = (unsigned char *) SEL_ADR(_cs, _eip);
    ssp = (unsigned short *) SEL_ADR(_ss, _esp);
    len = 0;
    switch (*csp) {
	case 0xca:			/* retf imm16 */
	case 0xcb:			/* retf */
	case 0xcf:			/* iret */
	    len = 1;
	    _eip = decode_use_16bit ? ssp[0] : ((unsigned int *) ssp)[0];
	    *segment = decode_use_16bit ? ssp[1] : ((unsigned int *) ssp)[1];
	    *sreg = cs_INDEX;
	    _esp += decode_use_16bit ? 4 : 8;
	    break;
    }
    if (!len)
	return 0;

    switch (*csp) {
	case 0xca:			/* retf imm16 */
	    len += 2;
	    _esp += ((unsigned short *) (csp + 1))[0];
	    break;
	case 0xcf:			/* iret */
	    _eflags = decode_use_16bit ? ssp[2] : ((unsigned int *) ssp)[2];
	    _esp += decode_use_16bit ? 2 : 4;
	    break;
    }
    if (len)
	D_printf("MSDOS: retf decoded, seg=0x%x\n", *segment);
    return len;
}

static int
decode_jmp_f(struct sigcontext_struct *scp, unsigned short *segment,
    unsigned char * sreg, int decode_use_16bit)
{
    unsigned short *ssp;
    unsigned char *csp;
    int len;

    csp = (unsigned char *) SEL_ADR(_cs, _eip);
    ssp = (unsigned short *) SEL_ADR(_ss, _esp);
    len = 0;
    switch (*csp) {
	case 0xea:			/* jmp seg:off16/off32 */
	    len = decode_use_16bit ? 5 : 7;
	    _eip = decode_use_16bit ? ((unsigned short *)(csp + 1))[0] :
		((unsigned int *)(csp + 1))[0];
	    *segment = decode_use_16bit ? ((unsigned short *)(csp + 3))[0] :
		((unsigned short *)(csp + 5))[0];
	    *sreg = cs_INDEX;
	    break;
    }
    if (len)
	D_printf("MSDOS: jmpf decoded, seg=0x%x\n", *segment);
    return len;
}

/*
 * decode_modify_segreg_insn tries to decode instructions which would modify a
 * segment register, returns the length of the insn.
 */
static  int
decode_modify_segreg_insn(struct sigcontext_struct *scp, unsigned
			  short *segment, unsigned char *sreg)
{
    unsigned char decode_use_16bit;
    unsigned char *csp;
    int len, size_prfix;

    csp = (unsigned char *) SEL_ADR(_cs, _eip);
    size_prfix = 0;
    decode_use_16bit = !Segments[_cs>>3].is_32;
    if (*csp == 0x66) { /* Operand-Size prefix */
	csp++;
	_eip++;
	decode_use_16bit ^= 1;
	size_prfix++;
    }
	
    /* first try pop sreg */
    if ((len = decode_pop_segreg(scp, segment, sreg))) {
      _esp += decode_use_16bit ? 2 : 4;
      _eip += len;
      return len+size_prfix;
    }

    /* try retf, iret */
    if ((len = decode_retf_iret(scp, segment, sreg, decode_use_16bit))) {
      /* eip, esp and eflags are modified! */
      return len+size_prfix;
    }

    /* try far jmp */
    if ((len = decode_jmp_f(scp, segment, sreg, decode_use_16bit))) {
      /* eip is modified! */
      return len+size_prfix;
    }

    return 0;
}
	
    
#if 0 /* Not USED!  JES 96/01/2x */
static  int msdos_fix_cs_prefix (struct sigcontext_struct *scp)
{
    unsigned char *csp;

    csp = (unsigned char *) SEL_ADR(_cs, _eip);
    if (*csp != 0x2e)		/* not cs prefix */
	return 0;

    /* bcc try to something like mov cs:[xx],ax here, we cheat it by */
    /* using mov gs:[xx],ax instead, hope bcc will never use gs :=( */

    if ((Segments[_cs>>3].type & MODIFY_LDT_CONTENTS_CODE) &&
	(Segments[(_cs>>3)+1].base_addr == Segments[_cs>>3].base_addr)
	&&((Segments[(_cs>>3)+1].type & MODIFY_LDT_CONTENTS_CODE)==0)) {
	    _gs = _cs + 8;
	    *csp = 0x65;	/* gs prefix */
	    return 1;
    }
    return 0;
}
#endif


int msdos_fault(struct sigcontext_struct *scp)
{
    struct sigcontext_struct new_sct;
    unsigned char reg;
    unsigned short segment, desc;
    unsigned long len;

    D_printf("MSDOS: msdos_fault, err=%#lx\n",_err);
    if ((_err & 0xffff) == 0) {	/*  not a selector error */
    /* Why should we "fix" the NULL dereferences? */
    /* Because the unmodified Win3.1 kernel (not WinOS2) needs this */
    /* Yes, but only when LDT is read-only, and then it doesn't work anyway.
     * So lets disable it again and see if someone else needs this. */
#if 0
	char fixed = 0;
	unsigned char * csp;

	csp = (unsigned char *) SEL_ADR(_cs, _eip);

	/* see if client wants to access control registers */
	if (*csp == 0x0f) {
	  if (cpu_trap_0f(csp, scp)) return 1;	/* 1=handled */
	}
	
	switch (*csp) {
	case 0x2e:		/* cs: */
	    break;		/* do nothing */
	case 0x36:		/* ss: */
	    break;		/* do nothing */
	case 0x26:		/* es: */
	    if (_es == 0) {
		D_printf("MSDOS: client tries to use use gdt 0 as es\n");
		_es = ConvertSegmentToDescriptor(0);
		fixed = 1;
	    }
	    break;
	case 0x64:		/* fs: */
	    if (_fs == 0) {
		D_printf("MSDOS: client tries to use use gdt 0 as fs\n");
		_fs = ConvertSegmentToDescriptor(0);
		fixed = 1;
	    }
	    break;
	case 0x65:		/* gs: */
	    if (_gs == 0) {
		D_printf("MSDOS: client tries to use use gdt 0 as es\n");
		_gs = ConvertSegmentToDescriptor(0);
		fixed = 1;
	    }
	    break;
	case 0xf2:		/* REPNE prefix */
	case 0xf3:		/* REP, REPE */
	    /* this might be a string insn */
	    switch (*(csp+1)) {
	    case 0xaa: case 0xab:		/* stos */
	    case 0xae: case 0xaf:	        /* scas */
		/* only use es */
		if (_es == 0) {
		    D_printf("MSDOS: client tries to use use gdt 0 as es\n");
		    _es = ConvertSegmentToDescriptor(0);
		    fixed = 1;
		}
		break;
	    case 0xa4: case 0xa5:		/* movs */
	    case 0xa6: case 0xa7:         /* cmps */
		/* use both ds and es */
		if (_es == 0) {
		    D_printf("MSDOS: client tries to use use gdt 0 as es\n");
		    _es = ConvertSegmentToDescriptor(0);
		    fixed = 1;
		}
		if (_ds == 0) {
		    D_printf("MSDOS: client tries to use use gdt 0 as ds\n");
		    _ds = ConvertSegmentToDescriptor(0);
		    fixed = 1;
		}
		break;
	    }
	    break;
	case 0x3e:		/* ds: */
	default:		/* assume default is using ds, but if the */
				/* client sets ss to 0, it is totally broken */
	    if (_ds == 0) {
		D_printf("MSDOS: client tries to use use gdt 0 as ds\n");
		_ds = ConvertSegmentToDescriptor(0);
		fixed = 1;
	    }
	    break;
	}
	return fixed;
#else
	return 0;
#endif
    }
    
    /* now it is a invalid selector error, try to fix it if it is */
    /* caused by an instruction such as mov Sreg,r/m16            */

    copy_context(&new_sct, scp);
    len = decode_modify_segreg_insn(&new_sct, &segment, &reg);

    if (len == 0) {
	/* simulate one instruction in instremu, and check which
	   segment register has changed;
	   pmode==2 means "simulate", count==1
	*/
	instr_emu(&new_sct, 2, 1);

	len = new_sct.eip - _eip;
	if (len == 0)
	    return 0;
	reg = 0xff;
	segment = 0xffff;
	if (_cs != new_sct.cs) {
	    reg = cs_INDEX;
	    segment = new_sct.cs;
	} else if (_ds != new_sct.ds) {
	    reg = ds_INDEX;
	    segment = new_sct.ds;
	} else if (_es != new_sct.es) {
	    reg = es_INDEX;
	    segment = new_sct.es;
	} else if (_fs != new_sct.fs) {
	    reg = fs_INDEX;
	    segment = new_sct.fs;
	} else if (_gs != new_sct.gs) {
	    reg = gs_INDEX;
	    segment = new_sct.gs;
	} else if (_ss != new_sct.ss) {
	    reg = ss_INDEX;
	    segment = new_sct.ss;
	}
	if (reg == 0xff)
	    return 0;
    }
    if (ValidAndUsedSelector(segment)) {
	/*
	 * The selector itself is OK, but the descriptor (type) is not.
	 * We cannot fix this! So just give up immediately and dont
	 * screw up the context.
	 */
	D_printf("MSDOS: msdos_fault: Illegal use of selector %#x\n", segment);
	return 0;
    }

    D_printf("MSDOS: try mov to a invalid selector 0x%04x\n", segment);

#if 0
    /* only allow using some special GTD\'s */
    if ((segment != 0x0040) && (segment != 0xa000) &&
	(segment != 0xb000) && (segment != 0xb800) &&
	(segment != 0xc000) && (segment != 0xe000) &&
	(segment != 0xf000) && (segment != 0xbf8) &&
	(segment != 0xf800) && (segment != 0xff00))
	return 0;
#endif    

    if (!(desc = (reg != cs_INDEX ? ConvertSegmentToDescriptor_lim(segment, 0xfffff) :
	ConvertSegmentToCodeDescriptor_lim(segment, 0xfffff))))
	return 0;

    /* OKay, all the sanity checks passed. Now we go and fix the selector */
    switch (reg) {
    case es_INDEX:
	new_sct.es = desc;
	break;
    case cs_INDEX:
	new_sct.cs = desc;
	break;
    case ss_INDEX:
	new_sct.ss = desc;
	break;
    case ds_INDEX:
	new_sct.ds = desc;
	break;
    case fs_INDEX:
	new_sct.fs = desc;
	break;
    case gs_INDEX:
	new_sct.gs = desc;
	break;
    }

    /* lets hope we fixed the thing, apply the "fix" to context and return */
    copy_context(scp, &new_sct);
    return 1;
}
