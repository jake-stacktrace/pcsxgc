/*  Pcsx - Pc Psx Emulator
 *  Copyright (C) 1999-2003  Pcsx Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <mach/vm_map.h>
#include <mach/mach_init.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/thread_status.h>
#include <mach/exception.h>
#include <mach/task.h>
#include <Kernel/mach/exc_server.h>
#include <pthread.h>

#include "PsxCommon.h"

#define FAST_DYNA_MEM

s8 *psxM;
s8 *psxP;
s8 *psxR;
s8 *psxH;
u32 *psxMemWLUT;
u32 *psxMemRLUT;

static int writeok=1;

static int sizeFromInstr(unsigned long instr)
{
	if ((instr>>26) == 0x1f) {
		// indexed operation
		switch (instr&0x7ff) {
			case 0x02E:
			case 0x42C:
			case 0x12E:
			case 0x52C:
			default:
				return 4;
			
			case 0x22E:
			case 0x62C:
			case 0x32E:
			case 0x72C:
				return 2;
			
			case 0x0AE:
			case 0x1AE:
				return 1;
		}
	} else {
#if 1
	switch ((instr>>26) & 0x28) {
		case 0x00:
			return 4;
		case 0x08:
			return 1;
		default:
			return 2;
	}
#else
	switch (instr>>26) {
		case 0x80>>2:
		case 0x84>>2:
		case 0x90>>2:
		case 0x94>>2:
			return 4;
		
		case 0xA0>>2:
		case 0xA4>>2:
		case 0xA8>>2:
		case 0xAC>>2:
		case 0xB0>>2:
		case 0xB4>>2:
			return 2;
		
		case 0x88>>2:
		case 0x8C>>2:
		case 0x98>>2:
		case 0x9C>>2:
			return 1;
	}
#endif
	}
}

#define MAX_EXCEPTION_PORTS 16

static struct {
    mach_msg_type_number_t count;
    exception_mask_t      masks[MAX_EXCEPTION_PORTS];
    exception_handler_t   ports[MAX_EXCEPTION_PORTS];
    exception_behavior_t  behaviors[MAX_EXCEPTION_PORTS];
    thread_state_flavor_t flavors[MAX_EXCEPTION_PORTS];
} old_exc_ports;

static pthread_t excThread = 0;
static mach_port_t exception_port;

#define MACH_CHECK_ERROR(name,res) \
if (res != KERN_SUCCESS) { \
    mach_error(#name, res); \
    exit(1); \
}

static void *exc_thread(void *foo)
{
	__Request__exception_raise_state_t request;
	__Reply__exception_raise_state_t reply;
	mach_msg_return_t res;

	for(;;)
	{
		res = mach_msg((mach_msg_header_t *)&request,
							MACH_RCV_MSG/*|MACH_RCV_LARGE*/,
							0,
							sizeof(request),
							exception_port,
							MACH_MSG_TIMEOUT_NONE,
							MACH_PORT_NULL);
		if (KERN_SUCCESS != res) {
			//if (MACH_RCV_PORT_CHANGED != res)
			//	mach_error("mach_msg", res);
			return (void *)0;
		}
	  
		if(exc_server((mach_msg_header_t *)&request, (mach_msg_header_t *)&reply)) {
			res = mach_msg_send((mach_msg_header_t *)&reply);
			MACH_CHECK_ERROR(mach_msg_send, res);
		}
	}
}

static kern_return_t forward_exception(
        mach_port_t thread,
        mach_port_t task,
        exception_type_t exception,
        exception_data_t data,
        mach_msg_type_number_t data_count)
{
	int i;
	kern_return_t r;
	mach_port_t port;
	exception_behavior_t behavior;
	thread_state_flavor_t flavor;
	
	thread_state_data_t thread_state;
	mach_msg_type_number_t thread_state_count = THREAD_STATE_MAX;
	
	for(i=0;i<old_exc_ports.count;i++)
		if(old_exc_ports.masks[i] & (1 << exception))
			break;
	if(i==old_exc_ports.count) 
		return KERN_FAILURE;
    
	port = old_exc_ports.ports[i];
	behavior = old_exc_ports.behaviors[i];
	flavor = old_exc_ports.flavors[i];

	if(behavior != EXCEPTION_DEFAULT) {
		r = thread_get_state(thread,flavor,thread_state,&thread_state_count);
		MACH_CHECK_ERROR (thread_get_state, r);
	}
	
	switch(behavior) {
		case EXCEPTION_DEFAULT:
			r = exception_raise(port,thread,task,exception,data,data_count);
			MACH_CHECK_ERROR (exception_raise, r);
			break;
		case EXCEPTION_STATE:
			r = exception_raise_state(port,thread,task,exception,data,
					data_count,&flavor,thread_state,thread_state_count,
					thread_state,&thread_state_count);
			MACH_CHECK_ERROR (exception_raise_state, r);
			break;
		case EXCEPTION_STATE_IDENTITY:
			r = exception_raise_state_identity(port,thread,task,exception,data,
					data_count,&flavor,thread_state,thread_state_count,
					thread_state,&thread_state_count);
			MACH_CHECK_ERROR (exception_raise_state_identity, r);
			break;
		default:
			break;
	}
	
	if(behavior != EXCEPTION_DEFAULT) {
		r = thread_set_state(thread,flavor,thread_state,thread_state_count);
		MACH_CHECK_ERROR (thread_set_state, r);
	}
	
	return r;
}

/* called from exc_server() */
kern_return_t catch_exception_raise(
		mach_port_t exception_port,
		mach_port_t thread,
		mach_port_t task,
		exception_type_t exception,
		exception_data_t code,
		mach_msg_type_number_t code_count)
{
	kern_return_t res;
	
	if(EXC_BAD_ACCESS == exception) {
		char *addr;
		thread_state_flavor_t flavor = PPC_EXCEPTION_STATE;
		mach_msg_type_number_t state_count = PPC_EXCEPTION_STATE_COUNT;
		ppc_exception_state_t exc_state;
		ppc_thread_state_t thread_state;
		
		res = thread_get_state(thread, flavor, (natural_t*)&exc_state, &state_count);
		if(KERN_SUCCESS != res) {
			exit(-1);
		}
		
		// get address causing the exception
		addr = (char*) exc_state.dar;
		
		if ((addr >= psxM) && (addr < (psxM+0x20000000))) {
			u32 mem = addr-psxM;
#if defined(PSXREC) && defined(FAST_DYNA_MEM)
			if (writeok && (mem < 0x00800000) && !Config.Cpu) {
				int i;
				u32 addr = (mem & 0x001fffff) & ~(4096-1);
				
				for (i=0; i<4096; i+=4)
					REC_CLEARM(addr+i);
				
				mprotect (psxM+addr, 4096, PROT_READ | PROT_WRITE);
				mprotect (psxM+addr+0x00200000, 4096, PROT_READ | PROT_WRITE);
				mprotect (psxM+addr+0x00400000, 4096, PROT_READ | PROT_WRITE);
				mprotect (psxM+addr+0x00600000, 4096, PROT_READ | PROT_WRITE);
				return KERN_SUCCESS;
			}
#endif
			// ignore the current instruction and increment pc
			flavor = PPC_THREAD_STATE;
			state_count = PPC_THREAD_STATE_COUNT;
			
			res = thread_get_state(thread, flavor, (natural_t*)&thread_state, &state_count);
			if(KERN_SUCCESS != res) {
				printf("failed to acquire thread state\n");
				exit(-1);
			}
			
			// Handle hardware registers
			if ((mem >> 16) == 0x1f80) {
				int isStoreOp = (KERN_PROTECTION_FAILURE != *code);
				unsigned long instr = *((unsigned long *)thread_state.srr0);
				long *reg = &((long *)&(thread_state.r0))[(instr >> 21) & 0x1f];
				int size = sizeFromInstr(instr);
				int swapped = !(((instr >> 26) == 0x1f) && (instr & 0x100));
				
/*				if (mem < 0x1f801000) {
					if (isStoreOp) {
						switch (size) {
							case 1: psxHu8ref(mem) = (u8)*reg; break;
							case 2: psxHu16ref(mem) = SWAPu16(*reg); break;
							case 4: psxHu32ref(mem) = SWAPu32(*reg);; break;
						}
					} else {
						switch (size) {
							case 1: *reg = (s8)psxHu8(mem);
							case 2: *reg = (s16)psxHu16(mem);
							case 4: *reg = (s32)psxHu32(mem);
						}
					}
				} else*/ {
					if (isStoreOp) {
						switch (size) {
							case 1: psxHwWrite8(mem, (u8)*reg); break;
							case 2: psxHwWrite16(mem, (u16)*reg); break;
							case 4: psxHwWrite32(mem, (u32)*reg); break;
						}
					} else {
						switch (size) {
							case 1: *reg = (u8)psxHwRead8(mem);
							case 2: *reg = (u16)(swapped ? psxHwRead16(mem) : SWAPu16(psxHwRead16(mem)));
							case 4: *reg = (u32)(swapped ? psxHwRead32(mem) : SWAPu32(psxHwRead32(mem)));
						}
					}
				}
			} else if (mem == 0x1ffe0130 && (KERN_PROTECTION_FAILURE == *code)) {
				unsigned long instr = *((unsigned long *)thread_state.srr0);
				int size = sizeFromInstr(instr);
				mem = thread_state.r3;
				if (size == 4 && mem == 0xfffe0130) {
					// writing 4 bytes to address 0xfffe0130
					u32 value = thread_state.r4;
					int i;

					switch (value) {
						case 0x800: case 0x804:
							if (writeok == 0) break;
							writeok = 0;
							memset(psxMemWLUT + 0x0000, 0, 0x80 * 4);
							memset(psxMemWLUT + 0x8000, 0, 0x80 * 4);
							memset(psxMemWLUT + 0xa000, 0, 0x80 * 4);
							mprotect(psxM, 0x800000, PROT_READ);
							break;
						case 0x1e988:
							if (writeok == 1) break;
							writeok = 1;
							for (i=0; i<0x80; i++) psxMemWLUT[i + 0x0000] = (u32)&psxM[(i & 0x1f) << 16];
							memcpy(psxMemWLUT + 0x8000, psxMemWLUT, 0x80 * 4);
							memcpy(psxMemWLUT + 0xa000, psxMemWLUT, 0x80 * 4);
							mprotect(psxM, 0x800000, PROT_READ|PROT_WRITE);
							break;
						default:
	#ifdef PSXMEM_LOG
							PSXMEM_LOG("unk %8.8lx = %x\n", mem, value);
	#endif
							break;
					}
				}
			}
			
			// increment pc
			thread_state.srr0 += 4;
			
			res = thread_set_state(thread, flavor, (natural_t*)&thread_state, state_count);
			if(KERN_SUCCESS != res) {
				exit(-1);
			}
		} else {
			return forward_exception(thread,task,exception,code,code_count);
		}
	} else if (EXC_BAD_INSTRUCTION == exception) {
		// unhandled for now
		return forward_exception(thread,task,exception,code,code_count);
	} else {
		// we don't want it anyway :)
		return forward_exception(thread,task,exception,code,code_count);
	}
	
	return KERN_SUCCESS;
}

void teardown_exc_listener()
{
	kern_return_t res;
	int i;
	
	if (excThread == 0)
		return;
	
	// do a nice cleanup
	for (i=0; i<old_exc_ports.count; i++) {
		res = thread_set_exception_ports(
				mach_thread_self(),
				old_exc_ports.masks[i],
				old_exc_ports.ports[i],
				old_exc_ports.behaviors[i],
				old_exc_ports.flavors[i]);
	}
	
	
	res = mach_port_destroy(mach_task_self(), exception_port);
	if (KERN_SUCCESS != res) {
		printf("failed to destroy exception port\n");
	}
	
	// and kill listener thread
	//pthread_kill(excThread, SIGKILL);
	//pthread_cancel(excThread
	
	excThread = 0;
}

int setup_exc_listener()
{
	kern_return_t res;
	mach_port_t mytask;
	pthread_attr_t attr;
	exception_mask_t mask = EXC_MASK_BAD_ACCESS|EXC_MASK_BAD_INSTRUCTION;
	
	teardown_exc_listener();
	
	mytask = mach_task_self();
	res = mach_port_allocate(mytask, MACH_PORT_RIGHT_RECEIVE, &exception_port);
	if(MACH_MSG_SUCCESS != res) {
		mach_error("mach_port_allocate", res);
		return -1;
	}
	
	res = mach_port_insert_right(mytask, exception_port, exception_port, MACH_MSG_TYPE_MAKE_SEND);
	if(MACH_MSG_SUCCESS != res) {
		mach_error("mach_port_insert_right", res);
		return -1;
	}
	
	res = task_get_exception_ports(
			mytask,
			mask,
			old_exc_ports.masks,
			&old_exc_ports.count,
			old_exc_ports.ports,
			old_exc_ports.behaviors,
			old_exc_ports.flavors);
	if(MACH_MSG_SUCCESS != res) {
		mach_error("task_get_exception_ports", res);
		return -1;
	}
	
	res = task_set_exception_ports(
			mytask,
			mask,
			exception_port,
			EXCEPTION_DEFAULT,
			MACHINE_THREAD_STATE);
	if(MACH_MSG_SUCCESS != res) {
		mach_error("task_set_exception_ports", res);
		return -1;
	}
	
	if(pthread_attr_init(&attr) != 0)
		return -1;
	if(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0) 
		return -1;
	
	if(pthread_create(&excThread,&attr,exc_thread,NULL) != 0)
		return -1;
	
	pthread_attr_destroy(&attr);
	
	return 0;
}

int psxMemInit() {
	mach_port_t mytask;
	mach_port_t handle_port = 0;
	mem_entry_name_port_t name_parent = 0;
	vm_size_t size;
	vm_offset_t offset;
	vm_prot_t perm = VM_PROT_READ|VM_PROT_WRITE;
	kern_return_t ret = 0;
	char *tmpMem[8];
	char *addr;
	int i, num;
	
	psxMemRLUT = (long*)malloc(0x10000 * 4);
	psxMemWLUT = (long*)malloc(0x10000 * 4);
	memset(psxMemRLUT, 0, 0x10000 * 4);
	memset(psxMemWLUT, 0, 0x10000 * 4);

#if 1
	mytask = mach_task_self();
	
	// try and find an appropiate memory segment
	// jump through a few hoops to get one that is at the end of the logical memory
	// to be correct we should make sure we're the only thread running, but this will
	// hopefully work 99.9% of the cases
	for (i=0; i<8; i++) {
		tmpMem[i] = NULL;
		ret = vm_allocate(mytask, (vm_address_t *)&tmpMem[i], 0x20000000, TRUE);
		if (ret != KERN_SUCCESS)
			break;
	}
	
	num = i;
	
	psxM = tmpMem[num-1];
	for (i=0; i<num; i++) {
		vm_deallocate(mytask, (vm_address_t)tmpMem[i], 0x20000000);
	}
	
	// segment found start the allocation
   ret = vm_allocate(mytask, (vm_address_t *)&psxM, 0x00200000, FALSE);
	if (ret != KERN_SUCCESS) {
		//printf("vm_allocate() failed: %i\n", ret);
		return -1;
	}

	offset = (vm_offset_t)psxM;
	size = 0x00200000;
	ret = mach_make_memory_entry(mytask, &size, offset, perm, &handle_port, name_parent);
	if(KERN_SUCCESS != ret) {
		vm_deallocate(mach_task_self(), (vm_address_t)psxM, 0x00200000);
		return -1;
	}
	
	for (i=1; i<4; i++) {
		addr = psxM+0x00200000*i;
		ret = vm_map(mytask, (vm_address_t *)&addr, 0x00200000, 0,
				FALSE, handle_port, 0, FALSE,
				VM_PROT_READ|VM_PROT_WRITE, VM_PROT_READ|VM_PROT_WRITE, VM_INHERIT_DEFAULT);
		if(KERN_SUCCESS != ret) {
			vm_deallocate(mach_task_self(), (vm_address_t)psxM, 0x00200000*i);
			return -1;
		}
	}
	
	// allocate the remaining linear memory segment
	addr = psxM + 0x00200000*i;
   ret = vm_allocate(mytask, (vm_address_t *)&addr, 0x20000000-0x00200000*i, FALSE);
	if(KERN_SUCCESS != ret) {
		vm_deallocate(mach_task_self(), (vm_address_t)psxM, 0x00200000*i);
		return -1;
	}
	
	psxP = psxM+0x1f000000;
	psxH = psxM+0x1f800000;
	psxR = psxM+0x1fc00000;

	// protect unused portions
	mprotect (psxM, 0x20000000, PROT_NONE);
	mprotect (psxM, 0x00800000, PROT_READ | PROT_WRITE);
	mprotect (psxP, 0x00010000, PROT_READ | PROT_WRITE);
	mprotect (psxH, 0x00010000, PROT_READ | PROT_WRITE);
//	mprotect (psxH, 0x00010000, PROT_NONE);
	mprotect (psxR, 0x00080000, PROT_READ);
/*
	// move the protected hw register segment
   ret = vm_allocate(mytask, (vm_address_t *)&psxH, 0x00010000, TRUE);
	if(KERN_SUCCESS != ret) {
		vm_deallocate(mach_task_self(), (vm_address_t)psxM, 0x20000000);
		return -1;
	}*/
#else
	// regular method
	psxM = (char*)malloc(0x00200000);
	psxP = (char*)malloc(0x00010000);
	psxH = (char*)malloc(0x00010000);
	psxR = (char*)malloc(0x00080000);
	if (psxMemRLUT == NULL || psxMemWLUT == NULL || 
		psxM == NULL || psxP == NULL || psxH == NULL) {
		SysMessage(_("Error allocating memory")); return -1;
	}
#endif
	
// MemR
	for (i=0; i<0x80; i++) psxMemRLUT[i + 0x0000] = (u32)&psxM[(i & 0x1f) << 16];
	memcpy(psxMemRLUT + 0x8000, psxMemRLUT, 0x80 * 4);
	memcpy(psxMemRLUT + 0xa000, psxMemRLUT, 0x80 * 4);

	for (i=0; i<0x01; i++) psxMemRLUT[i + 0x1f00] = (u32)&psxP[i << 16];

	for (i=0; i<0x01; i++) psxMemRLUT[i + 0x1f80] = (u32)&psxH[i << 16];

	for (i=0; i<0x08; i++) psxMemRLUT[i + 0xbfc0] = (u32)&psxR[i << 16];

// MemW
	for (i=0; i<0x80; i++) psxMemWLUT[i + 0x0000] = (u32)&psxM[(i & 0x1f) << 16];
	memcpy(psxMemWLUT + 0x8000, psxMemWLUT, 0x80 * 4);
	memcpy(psxMemWLUT + 0xa000, psxMemWLUT, 0x80 * 4);

	for (i=0; i<0x01; i++) psxMemWLUT[i + 0x1f00] = (u32)&psxP[i << 16];

	for (i=0; i<0x01; i++) psxMemWLUT[i + 0x1f80] = (u32)&psxH[i << 16];

	return setup_exc_listener();
}

void psxMemReset() {
	FILE *f = NULL;
	char Bios[256];
	
	// make sure we have write access to the right segments
	mprotect(psxM, 0x00200000, PROT_READ|PROT_WRITE);
	mprotect(psxR, 0x00080000, PROT_READ|PROT_WRITE);
	writeok = 1;

	memset(psxM, 0, 0x00200000);
	memset(psxP, 0, 0x00010000);
	memset(psxH, 0, 0x00010000);

	if (strcmp(Config.Bios, "HLE")) {
		sprintf(Bios, "%s%s", Config.BiosDir, Config.Bios);
		f = fopen(Bios, "rb");
		
		if (f == NULL) {
			SysMessage (_("Could not open bios:\"%s\". Enabling HLE Bios\n"), Bios);
			memset(psxR, 0, 0x00080000);
			Config.HLE = 1;
		}
		else {
			fread(psxR, 1, 0x80000, f);
			mprotect(psxR, 0x00080000, PROT_READ);
			fclose(f);
			Config.HLE = 0;
		}
	} else Config.HLE = 1;
}

void psxMemShutdown() {
	vm_deallocate(mach_task_self(), (vm_address_t)psxM, 0x02000000);
	//vm_deallocate(mach_task_self(), (vm_address_t)psxH, 0x00010000);

	free(psxMemRLUT);
	free(psxMemWLUT);
	
	teardown_exc_listener();
}

u8 psxMemRead8(u32 mem) {
	u32 t;

	t = mem - 0x1f800000;
	if (t >> 16 == 0) {
		if (t < 0x1000)
			return psxHu8(mem);
		else
			return psxHwRead8(mem);
	} else {
		return *(u8 *)(psxM + (mem & 0x1fffffff));
	}
}

u16 psxMemRead16(u32 mem) {
	u32 t;

	t = mem - 0x1f800000;
	if (t >> 16 == 0) {
		if (t < 0x1000)
			return psxHu16(mem);
		else
			return psxHwRead16(mem);
	} else {
		return SWAP16p((u16 *)(psxM + (mem & 0x1fffffff)));
	}
}

u32 psxMemRead32(u32 mem) {
	u32 t;
	
	t = mem - 0x1f800000;
	if (t >> 16 == 0) {
		if (t < 0x1000)
			return psxHu32(mem);
		else
			return psxHwRead32(mem);
	} else {
		return SWAP32p((u32 *)(psxM + (mem & 0x1fffffff)));
	}
}

#if 1
void psxMemWrite8(u32 mem, u8 value) {
	char *p;
	u32 t;

	t = mem >> 16;
	if (t == 0x1f80) {
		if (mem < 0x1f801000)
			psxHu8(mem) = value;
		else
			psxHwWrite8(mem, value);
	} else {
		p = (char *)(psxMemWLUT[t]);
		if (p != NULL) {
			*(u8  *)(p + (mem & 0xffff)) = value;
#if defined(PSXREC) && defined(FAST_DYNA_MEM)
			if (!Config.Cpu) REC_CLEARM(mem&(~3));
#endif
		} else {
#ifdef PSXMEM_LOG
			PSXMEM_LOG("err sb %8.8lx\n", mem);
#endif
		}
	}
}

void psxMemWrite16(u32 mem, u16 value) {
	char *p;
	u32 t;

	t = mem >> 16;
	if (t == 0x1f80) {
		if (mem < 0x1f801000)
			psxHu16ref(mem) = SWAPu16(value);
		else
			psxHwWrite16(mem, value);
	} else {
		p = (char *)(psxMemWLUT[t]);
		if (p != NULL) {
			*(u16 *)(p + (mem & 0xffff)) = SWAPu16(value);
#if defined(PSXREC) && defined(FAST_DYNA_MEM)
			if (!Config.Cpu) REC_CLEARM(mem&(~3));
#endif
		} else {
#ifdef PSXMEM_LOG
			PSXMEM_LOG("err sh %8.8lx\n", mem);
#endif
		}
	}
}

void psxMemWrite32(u32 mem, u32 value) {
	char *p;
	u32 t;

//	if ((mem&0x1fffff) == 0x71E18 || value == 0x48088800) SysPrintf("t2fix!!\n");
	t = mem >> 16;
	if (t == 0x1f80) {
		if (mem < 0x1f801000)
			psxHu32ref(mem) = SWAPu32(value);
		else
			psxHwWrite32(mem, value);
	} else {
		p = (char *)(psxMemWLUT[t]);
		if (p != NULL) {
			SWAP32wp((u32 *)(p + (mem & 0xffff)),value);
#if defined(PSXREC) && defined(FAST_DYNA_MEM)
			if (!Config.Cpu) REC_CLEARM(mem&(~3)); // assumes aligned writes - otherwise we might need to clear 2 recs
#endif
		} else {
			if (mem != 0xfffe0130) {
#if defined(PSXREC) && defined(FAST_DYNA_MEM)
				if (!writeok && !Config.Cpu) REC_CLEARM(mem);
#endif

#ifdef PSXMEM_LOG
				if (writeok) { PSXMEM_LOG("err sw %8.8lx\n", mem); }
#endif
			} else {
				int i;

				switch (value) {
					case 0x800: case 0x804:
						if (writeok == 0) break;
						writeok = 0;
						memset(psxMemWLUT + 0x0000, 0, 0x80 * 4);
						memset(psxMemWLUT + 0x8000, 0, 0x80 * 4);
						memset(psxMemWLUT + 0xa000, 0, 0x80 * 4);
						break;
					case 0x1e988:
						if (writeok == 1) break;
						writeok = 1;
						for (i=0; i<0x80; i++) psxMemWLUT[i + 0x0000] = (u32)&psxM[(i & 0x1f) << 16];
						memcpy(psxMemWLUT + 0x8000, psxMemWLUT, 0x80 * 4);
						memcpy(psxMemWLUT + 0xa000, psxMemWLUT, 0x80 * 4);
						break;
					default:
#ifdef PSXMEM_LOG
						PSXMEM_LOG("unk %8.8lx = %x\n", mem, value);
#endif
						break;
				}
			}
		}
	}
}
#else
void psxMemWrite8(u32 mem, u8 value) {
	u32 t;

	t = mem - 0x1f800000;
	if (t >> 16 == 0) {
		if (t < 0x1000)
			psxHu8(mem) = value;
		else
			psxHwWrite8(mem, value);
	} else {
		*(u8  *)(psxM + (mem & 0x1fffffff)) = value;
	}
}

void psxMemWrite16(u32 mem, u16 value) {
	u32 t;

	t = mem - 0x1f800000;
	if (t >> 16 == 0) {
		if (t < 0x1000)
			psxHu16ref(mem) = SWAPu16(value);
		else
			psxHwWrite16(mem, value);
	} else {
		*(u16 *)(psxM + (mem & 0x1ffffffe)) = SWAPu16(value);
	}
}

void psxMemWrite32(u32 mem, u32 value) {
	char *p;
	u32 t;
	
//	if ((mem&0x1fffff) == 0x71E18 || value == 0x48088800) SysPrintf("t2fix!!\n");
	t = mem - 0x1f800000;
	if (t >> 16 == 0) {
		if (t < 0x1000)
			psxHu32ref(mem) = SWAPu32(value);
		else
			psxHwWrite32(mem, value);
	} else {
		if (!(mem & ~0xbfffffff)) {
				SWAP32wp((u32 *)(psxM + (mem & 0x1ffffffc)),value);
		} else {
			if (mem != 0xfffe0130) {
#if defined(PSXREC) && defined(FAST_DYNA_MEM)
				if (!writeok && !Config.Cpu) REC_CLEARM(mem);
#endif

#ifdef PSXMEM_LOG
				if (writeok) { PSXMEM_LOG("err sw %8.8lx\n", mem); }
#endif
			} else {
				int i;

				switch (value) {
					case 0x800: case 0x804:
						if (writeok == 0) break;
						writeok = 0;
						mprotect(psxM, 0x200000, PROT_READ);
						//memset(psxMemWLUT + 0x0000, 0, 0x80 * 4);
						//memset(psxMemWLUT + 0x8000, 0, 0x80 * 4);
						//memset(psxMemWLUT + 0xa000, 0, 0x80 * 4);
						break;
					case 0x1e988:
						if (writeok == 1) break;
						writeok = 1;
						mprotect(psxM, 0x200000, PROT_READ|PROT_WRITE);
						//for (i=0; i<0x80; i++) psxMemWLUT[i + 0x0000] = (u32)&psxM[(i & 0x1f) << 16];
						//memcpy(psxMemWLUT + 0x8000, psxMemWLUT, 0x80 * 4);
						//memcpy(psxMemWLUT + 0xa000, psxMemWLUT, 0x80 * 4);
						break;
					default:
#ifdef PSXMEM_LOG
						PSXMEM_LOG("unk %8.8lx = %x\n", mem, value);
#endif
						break;
				}
			}
		}
	}
}
#endif

void *psxMemPointer(u32 mem) {
	u32 t;

	t = mem - 0x1f800000;
	if (t >> 16 == 0) {
		if (t < 0x1000)
			return (void *)&psxH[t];
		else
			return NULL;
	} else {
		return (void *)(psxM + (mem & 0x1fffffff));
	}
}
