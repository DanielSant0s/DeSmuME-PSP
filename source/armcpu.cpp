/*
	Copyright (C) 2006 yopyop
	Copyright (C) 2009-2015 DeSmuME team

	This file is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	This file is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with the this software.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <algorithm>

#include "armcpu.h"
#include "common.h"
#include "instructions.h"
#include "cp15.h"
#include "bios.h"
#include "debug.h"
#include "NDSSystem.h"
#include "MMU_timing.h"

#include"PSP/FrontEnd.h"
#ifdef HAVE_LUA
#include "lua-engine.h"
#endif
#ifdef HAVE_JIT
#include "arm_jit.h"
#endif
//#include "melib.h"

//#include "PSP/JobManager.h"



u32 _armcpu_prefetch(armcpu_t *armcpu) { 
	if(armcpu->proc_ID==0) return armcpu_prefetch<0>();
	else return armcpu_prefetch<1>();
}

u32 _armcpu_prefetch7() { 
	return armcpu_prefetch<1>();
}
u32 _armcpu_prefetch9() { 
	return armcpu_prefetch<0>();
}
 
extern int debuga;
 
armcpu_t NDS_ARM7;
armcpu_t NDS_ARM9;

#define SWAP(a, b, c) do      \
	              {       \
                         c=a; \
                         a=b; \
                         b=c; \
		      }       \
                      while(0)

#define STALLED_CYCLE_COUNT 10

static void
stall_cpu( void *instance) {
  armcpu_t *armcpu = (armcpu_t *)instance;
  //printf("STALL\n");
  armcpu->stalled = 1;
}
                      
static void
unstall_cpu( void *instance) {
  armcpu_t *armcpu = (armcpu_t *)instance;
  //printf("UNSTALL\n");
  armcpu->stalled = 0;
}

static void
install_post_exec_fn( void *instance,
                      void (*ex_fn)( void *, u32 adr, int thumb),
                      void *fn_data) {
  armcpu_t *armcpu = (armcpu_t *)instance;

  armcpu->post_ex_fn = ex_fn;
  armcpu->post_ex_fn_data = fn_data;
}

static void
remove_post_exec_fn( void *instance) {
  armcpu_t *armcpu = (armcpu_t *)instance;

  armcpu->post_ex_fn = NULL;
}

static u32 read_cpu_reg( void *instance, u32 reg_num)
{
	armcpu_t *armcpu = (armcpu_t *)instance;

	if ( reg_num <= 14) {
	  return armcpu->R[reg_num];
	}
	else if ( reg_num == 15) {
	  return armcpu->instruct_adr;
	}
	else if ( reg_num == 16) {
	  //CPSR
	  return armcpu->CPSR.val;
	}
	
	return 0;
}

static void
set_cpu_reg( void *instance, u32 reg_num, u32 value) {
  armcpu_t *armcpu = (armcpu_t *)instance;

  if ( reg_num <= 14) {
    armcpu->R[reg_num] = value;
  }
  else if ( reg_num == 15) {
    armcpu->next_instruction = value;
  }
  else if ( reg_num == 16) {
    /* FIXME: setting the CPSR */
  }
}

int armcpu_new( armcpu_t *armcpu, u32 id)
{
	armcpu->proc_ID = id;
	armcpu->stalled = 0;
	
	armcpu->base_mem_if.prefetch32 = NULL;
	armcpu->base_mem_if.prefetch16 = NULL;
	armcpu->base_mem_if.read8 = NULL;
	armcpu->base_mem_if.read16 = NULL;
	armcpu->base_mem_if.read32 = NULL;
	armcpu->base_mem_if.write8 = NULL;
	armcpu->base_mem_if.write16 = NULL;
	armcpu->base_mem_if.write32 = NULL;
	armcpu->base_mem_if.data = NULL;
	
	armcpu->SetControlInterface(&arm_default_ctrl_iface);
	armcpu->SetControlInterfaceData(armcpu);
	armcpu->SetCurrentMemoryInterface(NULL);
	armcpu->SetCurrentMemoryInterfaceData(NULL);
	
	armcpu->post_ex_fn = NULL;
	armcpu->post_ex_fn_data = NULL;
	
	armcpu_init(armcpu, 0);

	return 0;
}

void armcpu_t::SetControlInterface(const armcpu_ctrl_iface *theControlInterface)
{
	this->ctrl_iface = *theControlInterface;
}

armcpu_ctrl_iface* armcpu_t::GetControlInterface()
{
	return &this->ctrl_iface;
}

void armcpu_t::SetControlInterfaceData(void *theData)
{
	this->ctrl_iface.data = theData;
}

void* armcpu_t::GetControlInterfaceData()
{
	return this->ctrl_iface.data;
}

void armcpu_t::SetCurrentMemoryInterface(armcpu_memory_iface *theMemoryInterface)
{
	this->mem_if = theMemoryInterface;
}

armcpu_memory_iface* armcpu_t::GetCurrentMemoryInterface()
{
	return this->mem_if;
}

void armcpu_t::SetCurrentMemoryInterfaceData(void *theData)
{
	if (this->mem_if != NULL)
	{
		this->mem_if->data = theData;
	}
}

void* armcpu_t::GetCurrentMemoryInterfaceData()
{
	return (this->mem_if != NULL) ? this->mem_if->data : NULL;
}

void armcpu_t::SetBaseMemoryInterface(const armcpu_memory_iface *theMemInterface)
{
	this->base_mem_if = *theMemInterface;
}

armcpu_memory_iface* armcpu_t::GetBaseMemoryInterface()
{
	return &this->base_mem_if;
}

void armcpu_t::SetBaseMemoryInterfaceData(void *theData)
{
	this->base_mem_if.data = theData;
}

void* armcpu_t::GetBaseMemoryInterfaceData()
{
	return this->base_mem_if.data;
}

void armcpu_t::ResetMemoryInterfaceToBase()
{
	this->SetCurrentMemoryInterface(this->GetBaseMemoryInterface());
	this->SetCurrentMemoryInterfaceData(this->GetBaseMemoryInterfaceData());
}

//call this whenever CPSR is changed (other than CNVZQ or T flags); interrupts may need to be unleashed
void armcpu_t::changeCPSR()
{
	//but all it does is give them a chance to unleash by forcing an immediate reschedule
	//TODO - we could actually set CPSR through here and look for a change in the I bit
	//that would be a little optimization as well as a safety measure if we prevented setting CPSR directly
	NDS_Reschedule();
}

void armcpu_init(armcpu_t *armcpu, u32 adr)
{
#if defined(_M_X64) || defined(__x86_64__)
	memcpy(&armcpu->cond_table[0], &arm_cond_table[0], sizeof(arm_cond_table));
#endif

	//J_Init(false);
	
	armcpu->LDTBit = (armcpu->proc_ID==0); //set ARMv5 style bit--different for each processor
	armcpu->intVector = 0xFFFF0000 * (armcpu->proc_ID==0);
	armcpu->waitIRQ = FALSE;
	armcpu->halt_IE_and_IF = FALSE;
	armcpu->intrWaitARM_state = 0;

//#ifdef GDB_STUB
//    armcpu->irq_flag = 0;
//#endif

	for(int i = 0; i < 16; ++i)
		armcpu->R[i] = 0;
	
	armcpu->CPSR.val = armcpu->SPSR.val = SYS;
	
	armcpu->R13_usr = armcpu->R14_usr = 0;
	armcpu->R13_svc = armcpu->R14_svc = 0;
	armcpu->R13_abt = armcpu->R14_abt = 0;
	armcpu->R13_und = armcpu->R14_und = 0;
	armcpu->R13_irq = armcpu->R14_irq = 0;
	armcpu->R8_fiq = armcpu->R9_fiq = armcpu->R10_fiq = armcpu->R11_fiq = armcpu->R12_fiq = armcpu->R13_fiq = armcpu->R14_fiq = 0;
	
	armcpu->SPSR_svc.val = armcpu->SPSR_abt.val = armcpu->SPSR_und.val = armcpu->SPSR_irq.val = armcpu->SPSR_fiq.val = 0;

//#ifdef GDB_STUB
//    armcpu->instruct_adr = adr;
//	armcpu->R[15] = adr + 8;
//#else
	//armcpu->R[15] = adr;
//#endif

	//do something sensible when booting up to a thumb address
	armcpu->next_instruction = adr & ~1;
	armcpu->CPSR.bits.T = BIT0(adr);
	
//#ifndef GDB_STUB
	_armcpu_prefetch(armcpu);
//#endif
}

u32 armcpu_switchMode(armcpu_t *armcpu, u8 mode)
{
	u32 oldmode = armcpu->CPSR.bits.mode;
	
	switch(oldmode)
	{
		case USR :
		case SYS :
			armcpu->R13_usr = armcpu->R[13];
			armcpu->R14_usr = armcpu->R[14];
			break;
			
		case FIQ :
			{
                                u32 tmp;
				SWAP(armcpu->R[8], armcpu->R8_fiq, tmp);
				SWAP(armcpu->R[9], armcpu->R9_fiq, tmp);
				SWAP(armcpu->R[10], armcpu->R10_fiq, tmp);
				SWAP(armcpu->R[11], armcpu->R11_fiq, tmp);
				SWAP(armcpu->R[12], armcpu->R12_fiq, tmp);
				armcpu->R13_fiq = armcpu->R[13];
				armcpu->R14_fiq = armcpu->R[14];
				armcpu->SPSR_fiq = armcpu->SPSR;
				break;
			}
		case IRQ :
			armcpu->R13_irq = armcpu->R[13];
			armcpu->R14_irq = armcpu->R[14];
			armcpu->SPSR_irq = armcpu->SPSR;
			break;
			
		case SVC :
			armcpu->R13_svc = armcpu->R[13];
			armcpu->R14_svc = armcpu->R[14];
			armcpu->SPSR_svc = armcpu->SPSR;
			break;
		
		case ABT :
			armcpu->R13_abt = armcpu->R[13];
			armcpu->R14_abt = armcpu->R[14];
			armcpu->SPSR_abt = armcpu->SPSR;
			break;
			
		case UND :
			armcpu->R13_und = armcpu->R[13];
			armcpu->R14_und = armcpu->R[14];
			armcpu->SPSR_und = armcpu->SPSR;
			break;
		default :
			break;
		}
		
		switch(mode)
		{
			case USR :
			case SYS :
				armcpu->R[13] = armcpu->R13_usr;
				armcpu->R[14] = armcpu->R14_usr;
				//SPSR = CPSR;
				break;
				
			case FIQ :
				{
					u32 tmp;
					SWAP(armcpu->R[8], armcpu->R8_fiq, tmp);
					SWAP(armcpu->R[9], armcpu->R9_fiq, tmp);
					SWAP(armcpu->R[10], armcpu->R10_fiq, tmp);
					SWAP(armcpu->R[11], armcpu->R11_fiq, tmp);
					SWAP(armcpu->R[12], armcpu->R12_fiq, tmp);
					armcpu->R[13] = armcpu->R13_fiq;
					armcpu->R[14] = armcpu->R14_fiq;
					armcpu->SPSR = armcpu->SPSR_fiq;
					break;
				}
				
			case IRQ :
				armcpu->R[13] = armcpu->R13_irq;
				armcpu->R[14] = armcpu->R14_irq;
				armcpu->SPSR = armcpu->SPSR_irq;
				break;
				
			case SVC :
				armcpu->R[13] = armcpu->R13_svc;
				armcpu->R[14] = armcpu->R14_svc;
				armcpu->SPSR = armcpu->SPSR_svc;
				break;
				
			case ABT :
				armcpu->R[13] = armcpu->R13_abt;
				armcpu->R[14] = armcpu->R14_abt;
				armcpu->SPSR = armcpu->SPSR_abt;
				break;
				
          case UND :
				armcpu->R[13] = armcpu->R13_und;
				armcpu->R[14] = armcpu->R14_und;
				armcpu->SPSR = armcpu->SPSR_und;
				break;
				
			default :
				//printf("switchMode: WRONG mode %02X\n",mode);
				break;
	}
	
	armcpu->CPSR.bits.mode = mode & 0x1F;
	armcpu->changeCPSR();
	return oldmode;
}

u32 armcpu_Wait4IRQ(armcpu_t *cpu)
{
	cpu->waitIRQ = TRUE;
	cpu->halt_IE_and_IF = TRUE;
	return 1;
}
 
template<u32 PROCNUM>
FORCEINLINE static u32 armcpu_prefetch()
{
	armcpu_t* const armcpu = &ARMPROC;
//#ifdef GDB_STUB
//	u32 temp_instruction;
//#endif
	u32 curInstruction = armcpu->next_instruction;

	if(armcpu->CPSR.bits.T == 0)
	{
//#ifdef GDB_STUB
//		temp_instruction =
//			armcpu->mem_if->prefetch32( armcpu->mem_if->data,
//			armcpu->next_instruction);
//
//		if ( !armcpu->stalled) {
//			armcpu->instruction = temp_instruction;
//			armcpu->instruct_adr = armcpu->next_instruction;
//			armcpu->next_instruction += 4;
//			armcpu->R[15] = armcpu->next_instruction + 4;
//		}
//#else
		curInstruction &= 0xFFFFFFFC; //please don't change this to 0x0FFFFFFC -- the NDS will happily run on 0xF******* addresses all day long
		//please note that we must setup R[15] before reading the instruction since there is a protection
		//which prevents PC > 0x3FFF from reading the bios region
		armcpu->instruct_adr = curInstruction;
		armcpu->next_instruction = curInstruction + 4;
		armcpu->R[15] = curInstruction + 8;
		armcpu->instruction = _MMU_read32<PROCNUM, MMU_AT_CODE>(curInstruction);
//#endif

		return MMU_codeFetchCycles<PROCNUM,32>(curInstruction);
	}

//#ifdef GDB_STUB
//	temp_instruction =
//		armcpu->mem_if->prefetch16( armcpu->mem_if->data,
//		armcpu->next_instruction);
//
//	if ( !armcpu->stalled) {
//		armcpu->instruction = temp_instruction;
//		armcpu->instruct_adr = armcpu->next_instruction;
//		armcpu->next_instruction = armcpu->next_instruction + 2;
//		armcpu->R[15] = armcpu->next_instruction + 2;
//	}
//#else
	curInstruction &= 0xFFFFFFFE; //please don't change this to 0x0FFFFFFE -- the NDS will happily run on 0xF******* addresses all day long
	//please note that we must setup R[15] before reading the instruction since there is a protection
	//which prevents PC > 0x3FFF from reading the bios region
	armcpu->instruct_adr = curInstruction;
	armcpu->next_instruction = curInstruction + 2;
	armcpu->R[15] = curInstruction + 4;
	armcpu->instruction = _MMU_read16<PROCNUM, MMU_AT_CODE>(curInstruction);
//#endif

	if(PROCNUM==0)
	{
		// arm9 fetches 2 instructions at a time in thumb mode
		if(!(curInstruction == armcpu->instruct_adr + 2 && (curInstruction & 2)))
			return MMU_codeFetchCycles<PROCNUM,32>(curInstruction);
		else
			return 0;
	}

	return MMU_codeFetchCycles<PROCNUM,16>(curInstruction);
}


#if 0 /* not used */
static BOOL FASTCALL test_EQ(Status_Reg CPSR) { return ( CPSR.bits.Z); }
static BOOL FASTCALL test_NE(Status_Reg CPSR) { return (!CPSR.bits.Z); }
static BOOL FASTCALL test_CS(Status_Reg CPSR) { return ( CPSR.bits.C); }
static BOOL FASTCALL test_CC(Status_Reg CPSR) { return (!CPSR.bits.C); }
static BOOL FASTCALL test_MI(Status_Reg CPSR) { return ( CPSR.bits.N); }
static BOOL FASTCALL test_PL(Status_Reg CPSR) { return (!CPSR.bits.N); }
static BOOL FASTCALL test_VS(Status_Reg CPSR) { return ( CPSR.bits.V); }
static BOOL FASTCALL test_VC(Status_Reg CPSR) { return (!CPSR.bits.V); }
static BOOL FASTCALL test_HI(Status_Reg CPSR) { return (CPSR.bits.C) && (!CPSR.bits.Z); }
static BOOL FASTCALL test_LS(Status_Reg CPSR) { return (CPSR.bits.Z) || (!CPSR.bits.C); }
static BOOL FASTCALL test_GE(Status_Reg CPSR) { return (CPSR.bits.N==CPSR.bits.V); }
static BOOL FASTCALL test_LT(Status_Reg CPSR) { return (CPSR.bits.N!=CPSR.bits.V); }
static BOOL FASTCALL test_GT(Status_Reg CPSR) { return (!CPSR.bits.Z) && (CPSR.bits.N==CPSR.bits.V); }
static BOOL FASTCALL test_LE(Status_Reg CPSR) { return ( CPSR.bits.Z) || (CPSR.bits.N!=CPSR.bits.V); }
static BOOL FASTCALL test_AL(Status_Reg CPSR) { return 1; }

static BOOL (FASTCALL* test_conditions[])(Status_Reg CPSR)= {
	test_EQ , test_NE ,
	test_CS , test_CC ,
	test_MI , test_PL ,
	test_VS , test_VC ,
	test_HI , test_LS ,
	test_GE , test_LT ,
	test_GT , test_LE ,
	test_AL
};
#define TEST_COND2(cond, CPSR) \
	(cond<15&&test_conditions[cond](CPSR))
#endif

//TODO - merge with armcpu_irqException?
//http://www.ethernut.de/en/documents/arm-exceptions.html
//http://docs.google.com/viewer?a=v&q=cache:V4ht1YkxprMJ:www.cs.nctu.edu.tw/~wjtsai/EmbeddedSystemDesign/Ch3-1.pdf+arm+exception+handling&hl=en&gl=us&pid=bl&srcid=ADGEEShx9VTHbUhWdDOrTVRzLkcCsVfJiijncNDkkgkrlJkLa7D0LCpO8fQ_hhU3DTcgZh9rcZWWQq4TYhhCovJ625h41M0ZUX3WGasyzWQFxYzDCB-VS6bsUmpoJnRxAc-bdkD0qmsu&sig=AHIEtbR9VHvDOCRmZFQDUVwy53iJDjoSPQ
void armcpu_exception(armcpu_t *cpu, u32 number)
{
	Mode cpumode = USR;
	switch(number)
	{
	case EXCEPTION_RESET: cpumode = SVC; break;
	case EXCEPTION_UNDEFINED_INSTRUCTION: cpumode = UND; break;
	case EXCEPTION_SWI: cpumode = SVC; break;
	case EXCEPTION_PREFETCH_ABORT: cpumode = ABT; break;
	case EXCEPTION_DATA_ABORT: cpumode = ABT; break;
	case EXCEPTION_RESERVED_0x14: emu_halt(); break;
	case EXCEPTION_IRQ: cpumode = IRQ; break;
	case EXCEPTION_FAST_IRQ: cpumode = FIQ; break;
	}

	Status_Reg tmp = cpu->CPSR;
	armcpu_switchMode(cpu, cpumode);				//enter new mode
	cpu->R[14] = cpu->next_instruction;
	cpu->SPSR = tmp;							//save old CPSR as new SPSR
	cpu->CPSR.bits.T = 0;						//handle as ARM32 code
	cpu->CPSR.bits.I = 1;
	cpu->changeCPSR();
	cpu->R[15] = cpu->intVector + number;
	cpu->next_instruction = cpu->R[15];
	//printf("armcpu_exception!\n");
	//extern bool dolog;
	//dolog=true;

	//HOW DOES THIS WORTK WITHOUT A PREFETCH, LIKE IRQ BELOW?
	//I REALLY WISH WE DIDNT PREFETCH BEFORE EXECUTING
}

BOOL armcpu_irqException(armcpu_t *armcpu)
{
    Status_Reg tmp;

	//TODO - remove GDB specific code
//#ifdef GDB_STUB
//	armcpu->irq_flag = 0;
//#endif
      
	tmp = armcpu->CPSR;
	armcpu_switchMode(armcpu, IRQ);

	//TODO - remove GDB specific code
//#ifdef GDB_STUB
//	armcpu->R[14] = armcpu->next_instruction + 4;
//#else
	armcpu->R[14] = armcpu->instruct_adr + 4;
//#endif
	armcpu->SPSR = tmp;
	armcpu->CPSR.bits.T = 0;
	armcpu->CPSR.bits.I = 1;
	armcpu->next_instruction = armcpu->intVector + 0x18;
	armcpu->waitIRQ = 0;

	//must retain invariant of having next instruction to be executed prefetched
	//(yucky)
	_armcpu_prefetch(armcpu);

	return TRUE;
}

//TODO - remove GDB specific code
//BOOL
//armcpu_flagIrq( armcpu_t *armcpu) {
//  if(armcpu->CPSR.bits.I) return FALSE;
//
//  armcpu->waitIRQ = 0;
//
//#ifdef GDB_STUB
//  armcpu->irq_flag = 1;
//#endif
//
//  return TRUE;
//}

u32 TRAPUNDEF(armcpu_t* cpu)
{
	INFO("ARM%c: Undefined instruction: 0x%08X (%s) PC=0x%08X\n", cpu->proc_ID?'7':'9', cpu->instruction, decodeIntruction(false, cpu->instruction), cpu->instruct_adr);

	if (((cpu->intVector != 0) ^ (cpu->proc_ID == ARMCPU_ARM9)))
	{
		armcpu_exception(&NDS_ARM9,EXCEPTION_UNDEFINED_INSTRUCTION);
		return 4;
	}
	else
	{
		//emu_halt();
		return 4;
	}

}


int ARM7_ME(int data)
{
	// Usually, fetching and executing are processed parallelly.
	// So this function stores the cycles of each process to
	// the variables below, and returns appropriate cycle count.
	u32 cFetch = 0;
	u32 cExecute = 0;

	const int PROCNUM = 1;

	if (ARMPROC.CPSR.bits.T == 0)
	{
		if (
			CONDITION(ARMPROC.instruction) == 0x0E  //fast path for unconditional instructions
			|| (TEST_COND(CONDITION(ARMPROC.instruction), CODE(ARMPROC.instruction), ARMPROC.CPSR)) //handles any condition
			)
		{
			///	printf("%d\n", INSTRUCTION_INDEX(ARMPROC.instruction));
			cExecute = arm_instructions_set[PROCNUM][INSTRUCTION_INDEX(ARMPROC.instruction)](ARMPROC.instruction);

		}
		else
			cExecute = 1; // If condition=false: 1S cycle

		cFetch = armcpu_prefetch<PROCNUM>();

		return MMU_fetchExecuteCycles<PROCNUM>(cExecute, cFetch);
	}

	cExecute = thumb_instructions_set[PROCNUM][ARMPROC.instruction >> 6](ARMPROC.instruction);


	cFetch = armcpu_prefetch<PROCNUM>();

	return MMU_fetchExecuteCycles<PROCNUM>(cExecute, cFetch);
}

template<int PROCNUM>
u32 armcpu_execTFast(){
	u32 cExecute = thumb_instructions_set[PROCNUM][ARMPROC.instruction>>6](ARMPROC.instruction);
	u32 cFetch = armcpu_prefetch<PROCNUM>();
	return MMU_fetchExecuteCycles<PROCNUM>(cExecute, cFetch);
}

template<int PROCNUM>
u32 armcpu_execAFast()
{
	// Usually, fetching and executing are processed parallelly.
	// So this function stores the cycles of each process to
	// the variables below, and returns appropriate cycle count.
	u32 cExecute = arm_instructions_set[PROCNUM][INSTRUCTION_INDEX(ARMPROC.instruction)](ARMPROC.instruction);
	u32 cFetch = armcpu_prefetch<PROCNUM>();

	return MMU_fetchExecuteCycles<PROCNUM>(cExecute, cFetch);
}


template<int PROCNUM>
u32 armcpu_exec()
{
	// Usually, fetching and executing are processed parallelly.
	// So this function stores the cycles of each process to
	// the variables below, and returns appropriate cycle count.
	u32 cFetch = 0;
	u32 cExecute = 0;


	if(ARMPROC.CPSR.bits.T == 0)
	{
		if(
			CONDITION(ARMPROC.instruction) == 0x0E  //fast path for unconditional instructions
			|| (TEST_COND(CONDITION(ARMPROC.instruction), CODE(ARMPROC.instruction), ARMPROC.CPSR)) //handles any condition
			)
		{	
		///	printf("%d\n", INSTRUCTION_INDEX(ARMPROC.instruction));
			cExecute = arm_instructions_set[PROCNUM][INSTRUCTION_INDEX(ARMPROC.instruction)](ARMPROC.instruction);
		
		}
		else
		cExecute = 1; // If condition=false: 1S cycle

		cFetch = armcpu_prefetch<PROCNUM>();

		return MMU_fetchExecuteCycles<PROCNUM>(cExecute, cFetch);
	}

	cExecute = thumb_instructions_set[PROCNUM][ARMPROC.instruction>>6](ARMPROC.instruction);


	cFetch = armcpu_prefetch<PROCNUM>();

	return MMU_fetchExecuteCycles<PROCNUM>(cExecute, cFetch);
}

template<int PROCNUM>
u32 FastArmcpu_exec(u32 opcode)
{
	// Usually, fetching and executing are processed parallelly.
	// So this function stores the cycles of each process to
	// the variables below, and returns appropriate cycle count.

	//if(ARMPROC.CPSR.bits.T == 0)
	{
		if (
			 TEST_COND(CONDITION(opcode), CODE(opcode), ARMPROC.CPSR) //handles any condition
		   )
		{	
			return arm_instructions_set[PROCNUM][INSTRUCTION_INDEX(opcode)](opcode);
		
		}
		

		return 1;
	}
}

template u32 armcpu_exec<0>();
template u32 armcpu_exec<1>();

template u32 FastArmcpu_exec<0>(u32 opcode);
template u32 FastArmcpu_exec<1>(u32 opcode);

template u32 armcpu_execTFast<0>();
template u32 armcpu_execTFast<1>();

template u32 armcpu_execAFast<0>();
template u32 armcpu_execAFast<1>();

#ifdef HAVE_JIT
void arm_jit_sync()
{
	NDS_ARM7.next_instruction = NDS_ARM7.instruct_adr;
	NDS_ARM9.next_instruction = NDS_ARM9.instruct_adr;
	armcpu_prefetch<0>();
	armcpu_prefetch<1>();
}

template<int PROCNUM, bool jit>
u32 armcpu_exec()
{
	if (jit)
	{
		ARMPROC.instruct_adr &= ARMPROC.CPSR.bits.T?0xFFFFFFFE:0xFFFFFFFC;
		ArmOpCompiled f = (ArmOpCompiled)JIT_COMPILED_FUNC(ARMPROC.instruct_adr, PROCNUM);
		return f ? f() : arm_jit_compile<PROCNUM>();
	}

	return armcpu_exec<PROCNUM>();
}

template u32 armcpu_exec<0,false>();
template u32 armcpu_exec<0,true>();
template u32 armcpu_exec<1,false>();
template u32 armcpu_exec<1,true>();
#endif

void setIF(int PROCNUM, u32 flag)
{
	//don't set generated bits!!!
	//assert(!(flag&0x00200000));
	
	MMU.reg_IF_bits[PROCNUM] |= flag;
	
	NDS_Reschedule();
}

char* decodeIntruction(bool thumb_mode, u32 instr)
{
	char txt[20] = {0};
	u32 tmp = 0;
	if (thumb_mode == true)
	{
		tmp = (instr >> 6);
		strcpy(txt, intToBin((u16)tmp)+6);
	}
	else
	{
		tmp = ((instr >> 16) & 0x0FF0) | ((instr >> 4) & 0x0F);
		strcpy(txt, intToBin((u32)tmp)+20);
	}
	return strdup(txt);
}

const armcpu_ctrl_iface arm_default_ctrl_iface = {
	stall_cpu,
	unstall_cpu,
	read_cpu_reg,
	set_cpu_reg,
	install_post_exec_fn,
	remove_post_exec_fn,
	NULL
};
