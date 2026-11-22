#!/usr/bin/env python3
"""0.28.0 device and CPU-model regressions: VGA CRTC and DAC registers,
the CMOS clock, the absent game adapter and numeric coprocessor, INT3,
real-mode ARPL/SMSW/LOCK, DOS AH=60h (AH=37h stays refused) and AH=36h drive validation."""
import os
import subprocess
import tempfile
import dosrun_host_test as host

host.HARNESS = r'''
#include "cpu.h"
#include "dosapi.h"
#include "drives.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static struct cpu c;
static struct dos_state *d;
static void code(const unsigned char *p,size_t n) { c.sreg[1]=0x1000;c.ip=0;memcpy(c.mem+0x10000,p,n); }
#define CODE(...) do { unsigned char k[]={__VA_ARGS__}; code(k,sizeof(k)); } while(0)
static uint8_t in8(uint16_t port) { uint8_t v=0; CHECK(c.port_in8(&c,port,&v)==0); return v; }
static void out8(uint16_t port,uint8_t v) { CHECK(c.port_out8(&c,port,v)==0); }
static void dispatch(int vec) {
 push16(&c,c.flags); push16(&c,c.sreg[1]); push16(&c,c.ip);
 CHECK(dos_int(d,&c,vec)==CPU_OK);
 c.ip=pop16(&c); c.sreg[1]=pop16(&c); c.flags=pop16(&c);
}
static int isbcd(uint8_t v,unsigned max) { return (v&15)<=9 && (v>>4)<=9 && ((v>>4)*10+(v&15))<=max; }
int main(int argc,char **argv) {
 cpu_init(&c); c.sreg[2]=0x2000; c.r[SP]=0xFF00; c.flags=0x202;
 d=dos_new(&c); CHECK(d);

 /* CRTC: cursor location through index/data bytes and through a word
  * OUT (AL index to 3D4h, AH data to 3D5h, as two ISA byte cycles). */
 out8(0x3D4,0x0E); out8(0x3D5,0x01); out8(0x3D4,0x0F); out8(0x3D5,0x23);
 CHECK(rd16(&c,0x40,0x50)==((3<<8)|51));          /* 0123h = row 3, column 51 */
 out8(0x3D4,0x0E); CHECK(in8(0x3D5)==0x01); out8(0x3D4,0x0F); CHECK(in8(0x3D5)==0x23);
 CHECK(in8(0x3D4)==0x0F);
 CODE(0xBA,0xD4,0x03, 0xB8,0x0F,0x00, 0xEF, 0xB8,0x0E,0x00, 0xEF);  /* cursor 0000h */
 for(int i=0;i<5;i++) CHECK(cpu_step(&c)==CPU_OK);
 CHECK(rd16(&c,0x40,0x50)==0);
 /* Cursor shape: bit 5 of register 0Ah hides it, as INT 10h AH=01h. */
 out8(0x3D4,0x0A); out8(0x3D5,0x20); out8(0x3D4,0x0B); out8(0x3D5,0x0F);
 CHECK(rd16(&c,0x40,0x60)==0x200F);
 out8(0x3D4,0x0A); CHECK((in8(0x3D5)&0x3F)==0x20);
 /* A page-aligned start address selects that text page. */
 out8(0x3D4,0x0C); out8(0x3D5,0x08); out8(0x3D4,0x0D); out8(0x3D5,0x00);
 CHECK(rd8(&c,0x40,0x62)==1 && rd16(&c,0x40,0x4E)==0x1000);
 out8(0x3D4,0x0C); out8(0x3D5,0x00); CHECK(rd8(&c,0x40,0x62)==0);
 /* Timing registers read back; 0-7 obey the 11h write-protect bit. */
 out8(0x3D4,0x01); CHECK(in8(0x3D5)==0x4F);
 out8(0x3D4,0x11); out8(0x3D5,0x8E); out8(0x3D4,0x01); out8(0x3D5,0x33); CHECK(in8(0x3D5)==0x4F);
 out8(0x3D4,0x11); out8(0x3D5,0x0E); out8(0x3D4,0x01); out8(0x3D5,0x33); CHECK(in8(0x3D5)==0x33);
 out8(0x3D4,0x40); CHECK(in8(0x3D5)==0xFF);       /* no such register */
 puts("PASS: CRTC cursor location/shape, page start, timing read-back and write protect");

 /* DAC: default VGA palette, write/read index auto-increment, 6 bits. */
 out8(0x3C7,1); CHECK(in8(0x3C9)==0 && in8(0x3C9)==0 && in8(0x3C9)==42);
 out8(0x3C7,31); CHECK(in8(0x3C9)==63);
 out8(0x3C7,32); CHECK(in8(0x3C9)==0 && in8(0x3C9)==0 && in8(0x3C9)==63);   /* wheel start: blue */
 out8(0x3C7,40); CHECK(in8(0x3C9)==63 && in8(0x3C9)==0 && in8(0x3C9)==0);   /* red */
 out8(0x3C7,248); CHECK(in8(0x3C9)==0 && in8(0x3C9)==0 && in8(0x3C9)==0);
 out8(0x3C8,7); out8(0x3C9,1); out8(0x3C9,2); out8(0x3C9,0xFF); out8(0x3C9,9);
 CHECK(in8(0x3C8)==8);
 out8(0x3C7,7); CHECK(in8(0x3C9)==1 && in8(0x3C9)==2 && in8(0x3C9)==63 && in8(0x3C9)==9);
 CHECK(in8(0x3C7)==3); out8(0x3C8,0); CHECK(in8(0x3C7)==0);
 /* INT 10h AX=1010h/1015h agree with the ports. */
 c.r[AX]=0x1010; c.r[BX]=200; c.r[DX]=0x1100; c.r[CX]=0x2233; dispatch(0x10);
 out8(0x3C7,200); CHECK(in8(0x3C9)==0x11 && in8(0x3C9)==0x22 && in8(0x3C9)==0x33);
 c.r[AX]=0x1015; c.r[BX]=7; dispatch(0x10); CHECK((c.r[DX]>>8)==1 && c.r[CX]==0x023F);
 /* A mode set restores the default palette. */
 c.r[AX]=0x0003; dispatch(0x10); out8(0x3C7,7); CHECK(in8(0x3C9)==42 && in8(0x3C9)==42 && in8(0x3C9)==42);
 /* Attribute controller: 3DAh read resets the flip-flop; index then data. */
 (void)in8(0x3DA); out8(0x3C0,0x06); CHECK(in8(0x3C1)==0x14); out8(0x3C0,0x2A);
 (void)in8(0x3DA); out8(0x3C0,0x06); CHECK(in8(0x3C1)==0x2A); CHECK(in8(0x3C0)==0x06);
 /* No Sound Blaster / AdLib: the probe ports float high. */
 out8(0x226,1); out8(0x226,0); CHECK(in8(0x22A)==0xFF && in8(0x22E)==0xFF && in8(0x388)==0xFF);
 c.r[AX]=0x1A00; c.r[BX]=0xFFFF; dispatch(0x10); CHECK((c.r[AX]&255)==0x1A && c.r[BX]==0x0008);
 c.r[AX]=0x1200; c.r[BX]=0xFF10; dispatch(0x10); CHECK(c.r[BX]==0x0003);
 puts("PASS: DAC default palette, port index state machine, INT 10h palette calls");

 /* CMOS: BCD 24-hour clock, valid registers, NMI bit ignored. */
 out8(0x70,0x8B); CHECK(in8(0x71)==0x02);
 out8(0x70,0x0A); CHECK(!(in8(0x71)&0x80));
 out8(0x70,0x00); CHECK(isbcd(in8(0x71),59));
 out8(0x70,0x02); CHECK(isbcd(in8(0x71),59));
 out8(0x70,0x04); CHECK(isbcd(in8(0x71),23));
 out8(0x70,0x07); { uint8_t v=in8(0x71); CHECK(isbcd(v,31)&&v); }
 out8(0x70,0x08); { uint8_t v=in8(0x71); CHECK(isbcd(v,12)&&v); }
 out8(0x70,0x32); CHECK(isbcd(in8(0x71),99));
 out8(0x70,0x14); CHECK(!(in8(0x71)&2));            /* no coprocessor */
 { unsigned sum=0; for(int r=0x10;r<=0x2D;r++){out8(0x70,r);sum+=in8(0x71);}
   out8(0x70,0x2E); unsigned hi=in8(0x71); out8(0x70,0x2F); CHECK(((hi<<8)|in8(0x71))==sum); }
 c.r[AX]=0x0400; dispatch(0x1A); CHECK(!(c.flags&1) && isbcd(c.r[DX]>>8,12) && isbcd(c.r[DX]&255,31));
 c.r[AX]=0x0200; dispatch(0x1A); CHECK(!(c.flags&1) && isbcd(c.r[CX]>>8,23));
 c.r[AX]=0x2A00; dispatch(0x21); CHECK(c.r[CX]>=2020 && (c.r[DX]>>8)>=1 && (c.r[DX]>>8)<=12 && (c.r[AX]&255)<=6);
 puts("PASS: CMOS clock, checksum, INT 1Ah and DOS date share the wall clock");

 /* No game adapter; coprocessor ports are harmless. */
 CHECK(in8(0x201)==0xFF); out8(0x201,0); out8(0xF0,0); out8(0xF1,0);

 /* Absent coprocessor: the Intel presence probe sees nothing respond. */
 wr16(&c,0x1000,0x200,0x5A5A);
 CODE(0xDB,0xE3, 0xDD,0x3E,0x00,0x02, 0x9B, 0xD9,0x2E,0x00,0x02, 0xDC,0xC1);
 uint16_t ax=c.r[AX]; c.sreg[3]=0x1000;
 for(int i=0;i<5;i++) CHECK(cpu_step(&c)==CPU_OK);
 CHECK(c.ip==13 && rd16(&c,0x1000,0x200)==0x5A5A && c.r[AX]==ax);
 puts("PASS: no game adapter; ESC/FWAIT behave as on a PC without a coprocessor");

 /* Keyboard commands: typematic rate + parameter, LEDs, each ACKed. */
 out8(0x60,0xF3); CHECK(in8(0x64)==1 && in8(0x60)==0xFA); out8(0x60,0x20); CHECK(in8(0x60)==0xFA);
 out8(0x60,0xED); CHECK(in8(0x60)==0xFA); out8(0x60,0x07); CHECK(in8(0x60)==0xFA); CHECK(in8(0x64)==0);
 CHECK(c.port_out8(&c,0x60,0xFF)!=0);              /* reset is not modelled */
 /* FS/GS: prefixes, MOV, PUSH/POP and LSS/LFS/LGS. */
 wr16(&c,0x5000,0x10,0x1234); wr16(&c,0x1000,0x300,0x0010); wr16(&c,0x1000,0x302,0x5000); c.sreg[3]=0x1000;
 CODE(0x0F,0xB4,0x1E,0x00,0x03, 0x64,0x8B,0x07, 0x8C,0xE2, 0x0F,0xA0, 0x0F,0xA9, 0x8E,0xEA, 0x0F,0xB5,0x36,0x00,0x03);
 for(int i=0;i<7;i++) CHECK(cpu_step(&c)==CPU_OK);
 CHECK(c.fs==0x5000 && c.r[BX]==0x10 && c.r[AX]==0x1234 && c.r[DX]==0x5000 && c.gs==0x5000 && c.r[SI]==0x10);
 CODE(0x8E,0xF0); CHECK(cpu_step(&c)==CPU_FAULT);  /* segment register 6 */
 CODE(0x8E,0xC8); CHECK(cpu_step(&c)==CPU_FAULT);  /* MOV CS */
 c.sreg[3]=0x1000; CODE(0x0F,0xB2,0x26,0x00,0x03); CHECK(cpu_step(&c)==CPU_OK && c.sreg[2]==0x5000 && c.r[SP]==0x10);
 c.sreg[2]=0x2000; c.r[SP]=0xFF00;
 puts("PASS: keyboard command ACKs; FS/GS segments and LSS/LFS/LGS");
 /* INT3 enters vector 3 after the instruction. */
 wr16(&c,0,12,0x0040); wr16(&c,0,14,0x3000);
 CODE(0xCC); uint16_t sp=c.r[SP];
 CHECK(cpu_step(&c)==CPU_INT && c.int_no==3 && c.sreg[1]==0x3000 && c.ip==0x40);
 CHECK(rd16(&c,c.sreg[2],c.r[SP])==1); c.r[SP]=sp;
 /* ARPL: #UD to a program's own INT 6 handler, IP at the instruction;
  * with none installed the runner still refuses it. */
 CODE(0x63,0xC0); CHECK(cpu_step(&c)==CPU_FAULT && c.fault_opcode==0x63);
 wr16(&c,0,24,0x0080); wr16(&c,0,26,0x3000);
 CODE(0x63,0xC0); sp=c.r[SP];
 CHECK(cpu_step(&c)==CPU_INT && c.int_no==6 && c.ip==0x80 && rd16(&c,c.sreg[2],c.r[SP])==0);
 c.r[SP]=sp;
 /* SMSW: real mode, no coprocessor. Other group-7 forms are refused. */
 c.r[AX]=0xFFFF; CODE(0x0F,0x01,0xE0); CHECK(cpu_step(&c)==CPU_OK && c.r[AX]==0 && c.ip==3);
 wr16(&c,0x1000,0x200,0xFFFF); CODE(0x0F,0x01,0x26,0x00,0x02);
 CHECK(cpu_step(&c)==CPU_OK && rd16(&c,0x1000,0x200)==0);
 CODE(0x0F,0x01,0x06,0x00,0x02); CHECK(cpu_step(&c)==CPU_FAULT);   /* SGDT */
 CODE(0x0F,0x01,0xF0); CHECK(cpu_step(&c)==CPU_FAULT);             /* LMSW */
 /* LOCK: only memory read-modify-write forms. */
 wr16(&c,0x1000,0x200,41); CODE(0xF0,0xFF,0x06,0x00,0x02);
 CHECK(cpu_step(&c)==CPU_OK && rd16(&c,0x1000,0x200)==42 && c.ip==5);
 CODE(0xF0,0x01,0xC0); CHECK(cpu_step(&c)==CPU_FAULT && c.ip==0 && c.fault_opcode==0xF0);
 CODE(0xF0,0x8B,0x06,0x00,0x02); CHECK(cpu_step(&c)==CPU_FAULT);  /* MOV is not lockable */
 puts("PASS: INT3, real-mode ARPL #UD, SMSW, LOCK rules");

 /* DOS: switch character, TRUENAME, AH=36h rejects absent drives. */
 /* AH=37h stays unimplemented (see dosapi.c): invalid function. */
 c.r[AX]=0x3700; dispatch(0x21); CHECK((c.flags&1) && c.r[AX]==1);
 /* A TSR ends like AH=4Ch: nothing stays resident after dosrun. */
 c.r[AX]=0x3107; push16(&c,c.flags); push16(&c,c.sreg[1]); push16(&c,c.ip);
 CHECK(dos_int(d,&c,0x21)==CPU_EXIT && c.exit_code==7);
 c.r[SP]=(uint16_t)(c.r[SP]+6);
 push16(&c,c.flags); push16(&c,c.sreg[1]); push16(&c,c.ip);
 CHECK(dos_int(d,&c,0x27)==CPU_EXIT && c.exit_code==0); c.r[SP]=(uint16_t)(c.r[SP]+6);
 /* INT 2Fh: only multiplex 43h (XMS) answers; others return unchanged.
  * The XMS entry stub lives in the ROM area, not in program memory. */
 c.r[AX]=0x1600; dispatch(0x2F); CHECK(c.r[AX]==0x1600);
 c.r[AX]=0x4810; c.r[BX]=0x1234; dispatch(0x2F); CHECK(c.r[AX]==0x4810 && c.r[BX]==0x1234);
 c.r[AX]=0x4310; dispatch(0x2F); CHECK(c.sreg[0]==0xF000 && c.r[BX]>=0x380);
 /* Every BIOS/DOS vector points somewhere; a far call through an unset
  * one (INT 2Fh here) reaches dosrun's service and returns. */
 CHECK(rd16(&c,0,0x2F*4+2)==0xF000 && rd16(&c,0,0x60*4+2)==0);
 char root[]="/tmp/dosrun-device-XXXXXX"; CHECK(mkdtemp(root));
 char sub[64]; snprintf(sub,sizeof(sub),"%s/work",root); mkdir(sub,0700);
 /* dos_path lowercases whole host paths, so mount the (mixed-case)
  * temporary directory as "." after entering it. */
 CHECK(chdir(root)==0); dos_mount('C',"."); CHECK(dos_set_drive(2)==0); CHECK(dos_chdir("WORK")==0);
 c.sreg[3]=0x3000; c.sreg[0]=0x3000;
 const char *cases[][2]={{"file.txt","C:\\WORK\\FILE.TXT"},{"..\\a\\.\\b\\..\\c.dat","C:\\A\\C.DAT"},
  {"\\x/y","C:\\X\\Y"},{"c:..\\..","C:\\"},{"C:/Q","C:\\Q"}};
 for(unsigned k=0;k<5;k++){
  const char *p=cases[k][0]; uint16_t o=0x100; do wr8(&c,0x3000,o++,*p); while(*p++);
  c.r[SI]=0x100; c.r[DI]=0x400; c.r[AX]=0x6000; dispatch(0x21); CHECK(!(c.flags&1));
  char got[128]; for(int i=0;i<128;i++){got[i]=rd8(&c,0x3000,0x400+i); if(!got[i])break;}
  if(strcmp(got,cases[k][1])){fprintf(stderr,"%s -> %s\n",cases[k][0],got);exit(1);}
 }
 { const char *p="Q:\\X"; uint16_t o=0x100; do wr8(&c,0x3000,o++,*p); while(*p++); }
 c.r[SI]=0x100; c.r[AX]=0x6000; dispatch(0x21); CHECK((c.flags&1) && c.r[AX]==0x0F);
 c.r[AX]=0x3600; c.r[DX]=0; dispatch(0x21); CHECK(c.r[AX]!=0xFFFF);
 c.r[AX]=0x3600; c.r[DX]=17; dispatch(0x21); CHECK(c.r[AX]==0xFFFF);   /* Q: */
 CHECK(chdir("/")==0); rmdir(sub); rmdir(root);
 puts("PASS: DOS switch character refused, TRUENAME canonical paths, invalid-drive free space");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="dosrun-device-") as tmp:
    exe = host.build(tmp)
    subprocess.run([exe], check=True)
