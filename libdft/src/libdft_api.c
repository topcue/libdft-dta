/*-
 * Copyright (c) 2011, 2012, 2013, Columbia University
 * All rights reserved.
 *
 * This software was developed by Vasileios P. Kemerlis <vpk@cs.columbia.edu>
 * at Columbia University, New York, NY, USA, in June 2011.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Columbia University nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * 06/03/2011:
 * 	the array structure that kept the per-thread contexts has been
 * 	replaced by TLS-like logic for performance and safety reasons;
 * 	Vasileios P. Kemerlis(vpk@cs.columbia.edu)
 */

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "libdft_api.h"
#include "libdft_core.h"
#include "syscall_desc.h"
#include "tagmap.h"
#include "branch_pred.h"


/* 
 * thread context pointer (TLS emulation); we
 * spill a register for emulating TLS-like storage.
 * Specifically, thread_ctx_ptr shall hold the
 * address of a per-thread context structure
 */
REG thread_ctx_ptr;

/* syscall descriptors */
extern syscall_desc_t syscall_desc[SYSCALL_MAX];

/* ins descriptors */
ins_desc_t ins_desc[XED_ICLASS_LAST];

/* null_seg */
extern void *null_seg;

/*
 * thread start callback (analysis function)
 *
 * allocate space for the syscall context and VCPUs
 * (i.e., thread context), and set the TLS-like pointer
 * (i.e., thread_ctx_ptr) accordingly
 *
 * @tid:	thread id
 * @ctx:	CPU context
 * @flags:	OS specific flags for the new thread
 * @v:		callback value
 */
static void
thread_alloc(THREADID tid, CONTEXT *ctx, INT32 flags, VOID *v)
{
	/* thread context pointer (ptr) */
	thread_ctx_t *tctx = NULL;

	/* allocate space for the thread context; optimized branch */
	if (unlikely((tctx = (thread_ctx_t *)calloc(1,
					sizeof(thread_ctx_t))) == NULL)) {
		/* error message */
		LOG(string(__func__) + ": thread_ctx_t allocation failed (" +
				string(strerror(errno)) + ")\n");
		
		/* die */
		libdft_die();
	}

	/* save the address of the per-thread context to the spilled register */
	PIN_SetContextReg(ctx, thread_ctx_ptr, (ADDRINT)tctx);
}

/*
 * thread finish callback (analysis function)
 *
 * free the space for the syscall context and VCPUs
 *
 * @tid:	thread id
 * @ctx:	CPU context
 * @code:	OS specific termination code for the thread
 * @v:		callback value
 */
static void
thread_free(THREADID tid, const CONTEXT *ctx, INT32 code, VOID *v)
{
	/* get the thread context */
	thread_ctx_t *tctx = (thread_ctx_t *)
		PIN_GetContextReg(ctx, thread_ctx_ptr);

	/* free the allocated space */
	free(tctx);
}

/* 
 * syscall enter notification (analysis function)
 *
 * save the system call context and invoke the pre-syscall callback
 * function (if registered)
 *
 * @tid:	thread id
 * @ctx:	CPU context
 * @std:	syscall standard (e.g., Linux IA-32, IA-64, etc)
 * @v:		callback value
 */
static void
sysenter_save(THREADID tid, CONTEXT *ctx, SYSCALL_STANDARD std, VOID *v)
{
	/* get the thread context */
	thread_ctx_t *thread_ctx = (thread_ctx_t *)
		PIN_GetContextReg(ctx, thread_ctx_ptr);

	/* get the syscall number */
	size_t syscall_nr = PIN_GetSyscallNumber(ctx, std);

	/* unknown syscall; optimized branch */
	if (unlikely(syscall_nr >= SYSCALL_MAX)) {
		LOG(string(__func__) + ": unknown syscall (num=" +
				decstr(syscall_nr) + ")\n");
		/* syscall number is set to -1; hint for the sysexit_save() */
		thread_ctx->syscall_ctx.nr = -1;
		/* no context save and no pre-syscall callback invocation */
		return;
	}

	/* pass the system call number to sysexit_save() */
	thread_ctx->syscall_ctx.nr = syscall_nr;

	/*
	 * check if we need to save the arguments for that syscall
	 *
	 * we save only when we have a callback registered or the syscall
	 * returns a value in the arguments
	 */
	if (syscall_desc[syscall_nr].save_args |
		syscall_desc[syscall_nr].retval_args) {
		/*
		 * dump only the appropriate number of arguments
		 * or yet another lame way to avoid a loop (vpk)
		 */
		switch (syscall_desc[syscall_nr].nargs) {
			/* 6 */
			case SYSCALL_ARG5 + 1:
				thread_ctx->syscall_ctx.arg[SYSCALL_ARG5] =
					PIN_GetSyscallArgument(ctx,
								std,
								SYSCALL_ARG5);
			/* 5 */
			case SYSCALL_ARG4 + 1:
				thread_ctx->syscall_ctx.arg[SYSCALL_ARG4] =
					PIN_GetSyscallArgument(ctx,
								std,
								SYSCALL_ARG4);
			/* 4 */
			case SYSCALL_ARG3 + 1:
				thread_ctx->syscall_ctx.arg[SYSCALL_ARG3] =
					PIN_GetSyscallArgument(ctx,
								std,
								SYSCALL_ARG3);
			/* 3 */
			case SYSCALL_ARG2 + 1:
				thread_ctx->syscall_ctx.arg[SYSCALL_ARG2] =
					PIN_GetSyscallArgument(ctx,
								std,
								SYSCALL_ARG2);
			/* 2 */
			case SYSCALL_ARG1 + 1:
				thread_ctx->syscall_ctx.arg[SYSCALL_ARG1] =
					PIN_GetSyscallArgument(ctx,
								std,
								SYSCALL_ARG1);
			/* 1 */
			case SYSCALL_ARG0 + 1:
				thread_ctx->syscall_ctx.arg[SYSCALL_ARG0] =
					PIN_GetSyscallArgument(ctx,
								std,
								SYSCALL_ARG0);
			/* default */
			default:
				/* nothing to do */
				break;
		}

		/* 
		 * dump the architectural state of the processor;
		 * saved as "auxiliary" data
		 */
		thread_ctx->syscall_ctx.aux = ctx;

		/* call the pre-syscall callback (if any) */
		if (syscall_desc[syscall_nr].pre != NULL)
			syscall_desc[syscall_nr].pre(&thread_ctx->syscall_ctx);
	}
}

/* 
 * syscall exit notification (analysis function)
 *
 * save the system call context and invoke the post-syscall callback
 * function (if registered)
 *
 * NOTE: it performs tag cleanup for the syscalls that have side-effects in
 * their arguments
 *
 * @tid:	thread id
 * @ctx:	CPU context
 * @std:	syscall standard (e.g., Linux IA-32, IA-64, etc)
 * @v:		callback value
 */
static void
sysexit_save(THREADID tid, CONTEXT *ctx, SYSCALL_STANDARD std, VOID *v)
{
	/* iterator */
	size_t i;

	/* get the thread context */
	thread_ctx_t *thread_ctx = (thread_ctx_t *)
		PIN_GetContextReg(ctx, thread_ctx_ptr);

	/* get the syscall number */
	int syscall_nr = thread_ctx->syscall_ctx.nr;
	
	/* unknown syscall; optimized branch */
	if (unlikely(syscall_nr < 0)) {
		LOG(string(__func__) + ": unknown syscall (num=" +
				decstr(syscall_nr) + ")\n");
		/* no context save and no pre-syscall callback invocation */
		return;
	}
	
	/*
	 * check if we need to save the arguments for that syscall
	 *
	 * we save only when we have a callback registered or the syscall
	 * returns a value in the arguments
	 */
	if (syscall_desc[syscall_nr].save_args |
			syscall_desc[syscall_nr].retval_args) {
		/* dump only the appropriate number of arguments */
		thread_ctx->syscall_ctx.ret = PIN_GetSyscallReturn(ctx, std);

		/* 
		 * dump the architectural state of the processor;
		 * saved as "auxiliary" data
		 */
		thread_ctx->syscall_ctx.aux = ctx;

		/* thread_ctx->syscall_ctx.errno =
			PIN_GetSyscallErrno(ctx, std); */
	
		/* call the post-syscall callback (if any) */
		if (syscall_desc[syscall_nr].post != NULL)
			syscall_desc[syscall_nr].post(&thread_ctx->syscall_ctx);
		else {
			/* default post-syscall handling */

			/* 
			 * the syscall failed; typically 0 and positive
			 * return values indicate success
			 */
			if (thread_ctx->syscall_ctx.ret < 0)
				/* no need to do anything */
				return;

			/* traverse the arguments map */
			for (i = 0; i < syscall_desc[syscall_nr].nargs; i++)
				/* analyze each argument; optimized branch */
			if (unlikely(syscall_desc[syscall_nr].map_args[i] > 0)) 
				/* sanity check -- probably non needed */
				if (likely(
				(void *)thread_ctx->syscall_ctx.arg[i] != NULL))
				/* 
				 * argument i is changed by the system call;
				 * the length of the change is given by
				 * map_args[i]
				 */
				tagmap_clrn(thread_ctx->syscall_ctx.arg[i],
					syscall_desc[syscall_nr].map_args[i]);
		}
	}
}

/*
 * trace inspection (instrumentation function)
 *
 * traverse the basic blocks (BBLs) on the trace and
 * inspect every instruction for instrumenting it
 * accordingly
 *
 * @trace:      instructions trace; given by PIN
 * @v:		callback value
 */
static void
trace_inspect(TRACE trace, VOID *v)
{
	/* iterators */
	BBL bbl;
	INS ins;
	xed_iclass_enum_t ins_indx;

	/* traverse all the BBLs in the trace */
	for (bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
		/* traverse all the instructions in the BBL */
		for (ins = BBL_InsHead(bbl);
				INS_Valid(ins);
				ins = INS_Next(ins)) {
			        /*
				 * use XED to decode the instruction and
				 * extract its opcode
				 */
				ins_indx = (xed_iclass_enum_t)INS_Opcode(ins);

				/* 
				 * invoke the pre-ins instrumentation callback
				 */
				if (ins_desc[ins_indx].pre != NULL)
					ins_desc[ins_indx].pre(ins);

				/* 
				 * analyze the instruction (default handler)
				 */
				if (ins_desc[ins_indx].dflact == INSDFL_ENABLE)
					ins_inspect(ins);

				/* 
				 * invoke the post-ins instrumentation callback
				 */
				if (ins_desc[ins_indx].post != NULL)
					ins_desc[ins_indx].post(ins);
		}
	}
}

/*
 * initialize thread contexts
 *
 * spill a tool register for the thread contexts
 * and register a thread start callback
 *
 * returns: 0 on success, 1 on error
 */
static inline int
thread_ctx_init(void)
{
	/* claim a tool register; optimized branch */
	if (unlikely(
		(thread_ctx_ptr = PIN_ClaimToolRegister()) == REG_INVALID())) {
		/* error message */
		LOG(string(__func__) + ": register claim failed\n");

		/* failed */
		return 1;
	}

	/* 
	 * thread start/stop hooks;
	 * keep track of the threads and allocate/free space for the
	 * per-thread logistics (i.e., syscall context, VCPU, etc)
	 */
	PIN_AddThreadStartFunction(thread_alloc, NULL);
	PIN_AddThreadFiniFunction(thread_free,	NULL);

	/* success */
	return 0;
}

/*
 * global handler for internal errors (i.e., errors from libdft)
 *
 * handle memory protection (e.g., R/W/X access to null_seg)
 * 	-- or --
 * for unknown reasons, when an analysis function is executed,
 * the EFLAGS.AC bit (i.e., bit 18) is asserted, thus leading
 * into a runtime exception whenever an unaligned read/write
 * is performed from libdft. This callback can be registered
 * with PIN_AddInternalExceptionHandler() so as to trap the
 * generated signal and remediate
 *
 * @tid:		thread id		
 * @pExceptInfo:	exception descriptor
 * @pPhysCtxt:		physical processor state
 * @v:			callback value
 */
static EXCEPT_HANDLING_RESULT
excpt_hdlr(THREADID tid, EXCEPTION_INFO *pExceptInfo,
		PHYSICAL_CONTEXT *pPhysCtxt, VOID *v)
{
	/* memory violation address (fault) */
	ADDRINT vaddr = 0x0;

	/* unaligned memory accesses */
	if (PIN_GetExceptionCode(pExceptInfo) ==
			EXCEPTCODE_ACCESS_MISALIGNED) {
		/* clear EFLAGS.AC */
		PIN_SetPhysicalContextReg(pPhysCtxt, REG_EFLAGS,
			CLEAR_EFLAGS_AC(PIN_GetPhysicalContextReg(pPhysCtxt,
					REG_EFLAGS)));
		
		/* the exception is handled gracefully; commence execution */
		return EHR_HANDLED;
	}
	/* memory protection */
	else if (PIN_GetExceptionCode(pExceptInfo) ==
			EXCEPTCODE_ACCESS_DENIED) {
		
		/* get the address of the memory violation */	
		PIN_GetFaultyAccessAddress(pExceptInfo, &vaddr);
		
		/* sanity check */
		if (PAGE_ALIGN(vaddr) == (ADDRINT)null_seg) {
			/* error message */
			LOG(string(__func__) + ": invalid access -- " +
					"memory protection triggered\n");

			/* terminate the application */
			PIN_ExitApplication(-1);
		}
	}
	
	/* unknown exception; pass to the application */
	return EHR_UNHANDLED;
}

/*
 * initialization of the core tagging engine;
 * it must be called before using everything else
 *
 * returns: 0 on success, 1 on error
 */
int
libdft_init(void)
{
	/* initialize thread contexts; optimized branch */
	if (unlikely(thread_ctx_init()))
		/* thread contexts failed */
		return 1;

	/* initialize the tagmap; optimized branch */
	if (unlikely(tagmap_alloc()))
		/* tagmap initialization failed */
		return 1;
	
	/*
	 * syscall hooks; store the context of every syscall
	 * and invoke registered callbacks (if any)
	 */

	/* register sysenter_save() to be called before every syscall */
	PIN_AddSyscallEntryFunction(sysenter_save, NULL);
	
	/* register sysexit_save() to be called after every syscall */
	PIN_AddSyscallExitFunction(sysexit_save, NULL);
	
	/* initialize the ins descriptors */
	(void)memset(ins_desc, 0, sizeof(ins_desc));

	/* register trace_ins() to be called for every trace */
	TRACE_AddInstrumentFunction(trace_inspect, NULL);

	/* 
	 * register excpt_hdlr() to be called for handling internal
	 * (i.e., libdft or tool-related) exceptions
	 */
	PIN_AddInternalExceptionHandler(excpt_hdlr, NULL);
	
	/* success */
	return 0;
}

/*
 * stop the execution of the application inside the
 * tag-aware VM; the execution of the application
 * is not interrupted
 */
void
libdft_die(void)
{
       /*
        * detach Pin from the application;
        * the application will continue to execute natively
        */
       PIN_Detach();
}

/*
 * add a new pre-ins callback into an instruction descriptor
 *
 * @desc:	the ins descriptor
 * @pre:	function pointer to the pre-ins handler
 *
 * returns:	0 on success, 1 on error
 */
int
ins_set_pre(ins_desc_t *desc, void (* pre)(INS))
{
	/* sanity checks; optimized branch */
	if (unlikely((desc == NULL) | (pre == NULL)))
		/* return with failure */
		return 1;

	/* update the pre-ins callback */
	desc->pre = pre;

	/* success */
	return 0;
}

/*
 * add a new post-ins callback into an instruction descriptor
 *
 * @desc:	the ins descriptor
 * @post:	function pointer to the post-ins handler
 *
 * returns:	0 on success, 1 on error
 */
int
ins_set_post(ins_desc_t *desc, void (* post)(INS))
{
	/* sanity checks; optimized branch */
	if (unlikely((desc == NULL) | (post == NULL)))
		/* return with failure */
		return 1;

	/* update the post-ins callback */
	desc->post = post;

	/* success */
	return 0;
}

/*
 * remove the pre-ins callback from an instruction descriptor
 *
 * @desc:	the ins descriptor
 *
 * returns:     0 on success, 1 on error
 */
int
ins_clr_pre(ins_desc_t *desc)
{
	/* sanity check; optimized branch */
	if (unlikely(desc == NULL))
		/* return with failure */
		return 1;

	/* clear the pre-ins callback */
	desc->pre = NULL;

        /* return with success */
        return 0;
}

/*
 * remove the post-ins callback from an instruction descriptor
 *
 * @desc:	the ins descriptor
 *
 * returns:	0 on success, 1 on error
 */
int
ins_clr_post(ins_desc_t *desc)
{
	/* sanity check; optimized branch */
	if (unlikely(desc == NULL))
		/* return with failure */
		return 1;

	/* clear the post-ins callback */
	desc->post = NULL;

        /* return with success */
        return 0;
}

/*
 * set (enable/disable) the default action in an instruction descriptor
 *
 * @desc:       the ins descriptor
 *
 * returns:     0 on success, 1 on error
 */
int
ins_set_dflact(ins_desc_t *desc, size_t action)
{
	/* sanity checks */
	
	/* optimized branch */
	if (unlikely(desc == NULL))
		/* return with failure */
		return 1;

	switch (action) {
		/* valid actions */
		case INSDFL_ENABLE:
		case INSDFL_DISABLE:
			break;
		/* default handler */
		default:
			/* return with failure */
			return 1;
	}

	/* set the default action */
	desc->dflact = action;

        /* return with success */
        return 0;
}

/* 
 * REG-to-VCPU map;
 * get the register index in the VCPU structure
 * given a PIN register (32-bit regs)
 *
 * @reg:	the PIN register
 * returns:	the index of the register in the VCPU
 */
/* static inline */ size_t
REG32_INDX(REG reg)
{
	/* 
	 * differentiate based on the register;
	 * see mapping in struct vcpu_ctx_t in libdft_api.h
	 */
	switch (reg) {
		/* di */
		case LEVEL_BASE::REG::REG_EDI:
			return 0;
			/* not reached; safety */
			break;
		/* si */
		case LEVEL_BASE::REG::REG_ESI:
			return 1;
			/* not reached; safety */
			break;
		/* bp */
		case LEVEL_BASE::REG::REG_EBP:
			return 2;
			/* not reached; safety */
			break;
		/* sp */
		case LEVEL_BASE::REG::REG_ESP:
			return 3;
			/* not reached; safety */
			break;
		/* bx */
		case LEVEL_BASE::REG::REG_EBX:
			return 4;
			/* not reached; safety */
			break;
		/* dx */
		case LEVEL_BASE::REG::REG_EDX:
			return 5;
			/* not reached; safety */
			break;
		/* cx */
		case LEVEL_BASE::REG::REG_ECX:
			return 6;
			/* not reached; safety */
			break;
		/* ax */
		case LEVEL_BASE::REG::REG_EAX:
			return 7;
			/* not reached; safety */
			break;
		default:
			/* 
			 * paranoia;
			 * unknown 32-bit registers are mapped
			 * to the scratch register of the VCPU
			 */
			//return 8;
			assert(reg < LEVEL_BASE::REG::REG_LAST);
			return GRP_NUM + reg;
	}
}

/* 
 * REG-to-VCPU map;
 * get the register index in the VCPU structure
 * given a PIN register (16-bit regs)
 *
 * @reg:	the PIN register
 * returns:	the index of the register in the VCPU
 */
/* static inline */ size_t
REG16_INDX(REG reg)
{
	/* 
	 * differentiate based on the register;
	 * we map the 16-bit registers to their 32-bit
	 * containers (e.g., AX -> EAX)
	 */
	switch (reg) {
		/* di */
		case REG_DI:
			return 0;
			/* not reached; safety */
			break;
		/* si */
		case REG_SI:
			return 1;
			/* not reached; safety */
			break;
		/* bp */
		case REG_BP:
			return 2;
			/* not reached; safety */
			break;
		/* sp */
		case REG_SP:
			return 3;
			/* not reached; safety */
			break;
		/* bx */
		case REG_BX:
			return 4;
			/* not reached; safety */
			break;
		/* dx */
		case REG_DX:
			return 5;
			/* not reached; safety */
			break;
		/* cx */
		case REG_CX:
			return 6;
			/* not reached; safety */
			break;
		/* ax */
		case REG_AX:
			return 7;
			/* not reached; safety */
			break;
		default:
			/* 
			 * paranoia;
			 * unknown 16-bit registers are mapped
			 * to the scratch register of the VCPU
			 */
			//return 8;
			assert(reg < LEVEL_BASE::REG::REG_LAST);
			return GRP_NUM + reg;
	}
}

/* 
 * REG-to-VCPU map;
 * get the register index in the VCPU structure
 * given a PIN register (8-bit regs)
 *
 * @reg:	the PIN register
 * returns:	the index of the register in the VCPU
 */
/* static inline */ size_t
REG8_INDX(REG reg)
{
	/* 
	 * differentiate based on the register;
	 * we map the 8-bit registers to their 32-bit
	 * containers (e.g., AH -> EAX)
	 */
	switch (reg) {
		/* ah/al */
		case REG_AH:
		case REG_AL:
			return 7;
			/* not reached; safety */
			break;
		/* ch/cl */
		case REG_CH:
		case REG_CL:
			return 6;
			/* not reached; safety */
			break;
		/* dh/dl */
		case REG_DH:
		case REG_DL:
			return 5;
			/* not reached; safety */
			break;
		/* bh/bl */
		case REG_BH:
		case REG_BL:
			return 4;
			/* not reached; safety */
			break;
		default:
			/* 
			 * paranoia;
			 * unknown 8-bit registers are mapped
			 * to the scratch register
			 */
			//return 8;
			assert(reg < LEVEL_BASE::REG::REG_LAST);
			return GRP_NUM + reg;
	}
}
