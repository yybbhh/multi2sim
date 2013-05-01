/*
 *  Multi2Sim
 *  Copyright (C) 2012  Rafael Ubal (ubal@ece.neu.edu)
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

#include <assert.h>

#include <arch/southern-islands/asm/bin-file.h>
#include <arch/southern-islands/emu/ndrange.h>
#include <arch/southern-islands/timing/gpu.h>
#include <arch/x86/emu/context.h>
#include <arch/x86/emu/emu.h>
#include <arch/x86/emu/regs.h>
#include <lib/mhandle/mhandle.h>
#include <lib/util/debug.h>
#include <lib/util/list.h>
#include <lib/util/misc.h>
#include <lib/util/string.h>
#include <mem-system/memory.h>

#include "opencl.h"
#include "si-kernel.h"
#include "si-program.h"

#define SI_DRIVER_MAX_WORK_GROUP_BUFFER_SIZE 16

// XXX This needs to be renamed and set when the simulator starts up.  Leaving
// it here in the driver for now to avoid touching m2s.c
int fused_device = 0;

static char *opencl_err_abi_call =
	"\tAn invalid function code was generated by the your application as\n"
	"\tan argument of a system call reserved for the Multi2Sim OpenCL\n"
	"\tRuntime library. Please recompile your application and try again.\n";

struct opencl_abi_si_driver_state_t
{
	struct opencl_si_kernel_t *kernel;
	struct si_ndrange_t *ndrange;
	int ready_for_work;
	int wait_for_ndrange_completion;
	int ndrange_complete;
};

struct opencl_abi_si_driver_state_t driver_state;


/* List of OpenCL Runtime calls */
enum opencl_abi_call_t
{
	opencl_abi_invalid = 0,
#define OPENCL_ABI_CALL(name, code) opencl_abi_##name = code,
#include "opencl.dat"
#undef OPENCL_ABI_CALL
	opencl_abi_call_count
};


/* List of OpenCL ABI call names */
char *opencl_abi_call_name[opencl_abi_call_count + 1] =
{
	NULL,
#define OPENCL_ABI_CALL(name, code) #name,
#include "opencl.dat"
#undef OPENCL_ABI_CALL
	NULL
};

/* Forward declarations of OpenCL Runtime functions */
#define OPENCL_ABI_CALL(name, code) \
	static int opencl_abi_##name##_impl(struct x86_ctx_t *ctx);
#include "opencl.dat"
#undef OPENCL_ABI_CALL


/* List of OpenCL Runtime functions */
typedef int (*opencl_abi_call_t)(struct x86_ctx_t *ctx);
static opencl_abi_call_t opencl_abi_call_table[opencl_abi_call_count + 1] =
{
	NULL,
#define OPENCL_ABI_CALL(name, code) opencl_abi_##name##_impl,
#include "opencl.dat"
#undef OPENCL_ABI_CALL
	NULL
};


/* Debug */
int opencl_debug_category;



int opencl_abi_call(struct x86_ctx_t *ctx)
{
	struct x86_regs_t *regs = ctx->regs;

	/* Variables */
	int code;
	int ret;

	/* Function code */
	code = regs->ebx;
	if (code <= opencl_abi_invalid || code >= opencl_abi_call_count)
		fatal("%s: invalid OpenCL ABI call (code %d).\n%s",
			__FUNCTION__, code, opencl_err_abi_call);

	/* Debug */
	opencl_debug("OpenCL ABI call '%s' (code %d)\n",
		opencl_abi_call_name[code], code);

	/* Call OpenCL Runtime function */
	assert(opencl_abi_call_table[code]);
	ret = opencl_abi_call_table[code](ctx);

	/* Return value */
	return ret;
}


void opencl_init(void)
{
}


void opencl_done(void)
{
	opencl_si_program_list_done();
	opencl_si_kernel_list_done();
}




/*
 * OpenCL ABI call #1 - init
 *
 * Returns the version of the OpenCL driver.
 *
 * @param struct opencl_version_t *version
 *
 * 	The driver returns its version information in this argument, with a
 * 	data structure formed of two integer fields:
 *
 * 	struct opencl_version_t
 * 	{
 * 		int major;
 * 		int minor;
 * 	};
 *
 * @return
 *	The function always returns 0.
 */

/* NOTE: when modifying the values of these two macros, the same values should
 * be reflected in 'runtime/opencl/platform.c'. */
#define OPENCL_VERSION_MAJOR  1
#define OPENCL_VERSION_MINOR  1559

struct opencl_version_t
{
	int major;
	int minor;
};

static int opencl_abi_init_impl(struct x86_ctx_t *ctx)
{
	struct x86_regs_t *regs = ctx->regs;
	struct mem_t *mem = ctx->mem;

	unsigned int version_ptr;
	struct opencl_version_t version;

	/* Arguments */
	version_ptr = regs->ecx;
	opencl_debug("\tversion_ptr=0x%x\n", version_ptr);

	/* Return version */
	assert(sizeof(struct opencl_version_t) == 8);
	version.major = OPENCL_VERSION_MAJOR;
	version.minor = OPENCL_VERSION_MINOR;
	mem_write(mem, version_ptr, sizeof version, &version);
	opencl_debug("\tMulti2Sim OpenCL implementation in host: v. %d.%d.\n",
		OPENCL_VERSION_MAJOR, OPENCL_VERSION_MINOR);
	opencl_debug("\tMulti2Sim OpenCL Runtime in guest: v. %d.%d.\n",
		version.major, version.minor);

	/* Return success */
	return 0;
}




/*
 * OpenCL ABI call #2 - si_mem_alloc
 *
 * Allocates memory in the Southern Islands device.
 *
 * @param unsigned int size
 *
 * 	Number of bytes to allocate.
 *
 * @return void *
 *
 *	The function returns a pointer in the device memory space. This pointer
 *	should not be dereferenced in the runtime, but instead passed to other
 *	ABI calls taking device pointers as input arguments.
 */

static int opencl_abi_si_mem_alloc_impl(struct x86_ctx_t *ctx)
{
	struct x86_regs_t *regs = ctx->regs;

	unsigned int size;
	unsigned int device_ptr;

	/* Arguments */
	size = regs->ecx;
	opencl_debug("\tsize = %u\n", size);

	/* For now, memory allocation in device memory is done by just 
	 * incrementing a pointer to the top of the global memory space. 
	 * Since memory deallocation is not implemented, "holes" in the 
	 * memory space are not considered. */
	device_ptr = si_emu->video_mem_top;
	si_emu->video_mem_top += size;
	opencl_debug("\t%d bytes of device memory allocated at 0x%x\n",
			size, device_ptr);

	/* Return device pointer */
	return device_ptr;
}




/*
 * OpenCL ABI call #3 - si_mem_read
 *
 * Read memory from Southern Islands device into host memory space.
 *
 * @param void *host_ptr
 *
 * 	Destination pointer in host memory space.
 *
 * @param void *device_ptr
 *
 * 	Source pointer in device memory space.
 *
 * @param unsigned int size
 *
 * 	Number of bytes to read.
 *
 * @return void
 *
 *	The function does not have any return value.
 */

static int opencl_abi_si_mem_read_impl(struct x86_ctx_t *ctx)
{
	struct x86_regs_t *regs = ctx->regs;
	struct mem_t *mem = ctx->mem;

	unsigned int host_ptr;
	unsigned int device_ptr;
	unsigned int size;

	void *buf;

	/* Arguments */
	host_ptr = regs->ecx;
	device_ptr = regs->edx;
	size = regs->esi;
	opencl_debug("\thost_ptr = 0x%x, device_ptr = 0x%x, size = %d bytes\n",
			host_ptr, device_ptr, size);

	/* Check memory range */
	if (device_ptr + size > si_emu->video_mem_top)
		fatal("%s: accessing device memory not allocated",
				__FUNCTION__);

	/* Read memory from device to host */
	buf = xmalloc(size);
	mem_read(si_emu->video_mem, device_ptr, size, buf);
	mem_write(mem, host_ptr, size, buf);
	free(buf);

	/* Return */
	return 0;
}




/*
 * OpenCL ABI call #4 - si_mem_write
 *
 * Write memory from host into Southern Islands device.
 *
 * @param void *device_ptr
 *
 * 	Destination pointer in device memory.
 *
 * @param void *host_ptr
 *
 * 	Source pointer in host memory.
 *
 * @param unsigned int size
 *
 * 	Number of bytes to read.
 *
 * @return void
 *
 *	The function does not have any return value.
 */

static int opencl_abi_si_mem_write_impl(struct x86_ctx_t *ctx)
{
	struct x86_regs_t *regs = ctx->regs;
	struct mem_t *mem = ctx->mem;

	unsigned int device_ptr;
	unsigned int host_ptr;
	unsigned int size;

	void *buf;

	/* Arguments */
	device_ptr = regs->ecx;
	host_ptr = regs->edx;
	size = regs->esi;
	opencl_debug("\tdevice_ptr = 0x%x, host_ptr = 0x%x, size = %d bytes\n",
			device_ptr, host_ptr, size);

	/* Check memory range */
	if (device_ptr + size > si_emu->video_mem_top)
		fatal("%s: accessing device memory not allocated",
				__FUNCTION__);

	/* Write memory from host to device */
	buf = xmalloc(size);
	mem_read(mem, host_ptr, size, buf);
	mem_write(si_emu->video_mem, device_ptr, size, buf);
	free(buf);

	/* Return */
	return 0;
}




/*
 * OpenCL ABI call #5 - si_mem_copy
 *
 * Copy memory across two different regions of the Southern Islands device
 * memory space.
 *
 * @param void *dest_ptr
 *
 * 	Destination pointer in device memory.
 *
 * @param void *src_ptr
 *
 * 	Source pointer in device memory.
 *
 * @param unsigned int size
 *
 * 	Number of bytes to read.
 *
 * @return void
 *
 *	The function does not have any return value.
 */

static int opencl_abi_si_mem_copy_impl(struct x86_ctx_t *ctx)
{
	struct x86_regs_t *regs = ctx->regs;

	unsigned int dest_ptr;
	unsigned int src_ptr;
	unsigned int size;

	void *buf;

	/* Arguments */
	dest_ptr = regs->ecx;
	src_ptr = regs->edx;
	size = regs->esi;
	opencl_debug("\tdest_ptr = 0x%x, src_ptr = 0x%x, size = %d bytes\n",
			dest_ptr, src_ptr, size);

	/* Check memory range */
	if (src_ptr + size > si_emu->video_mem_top ||
			dest_ptr + size > si_emu->video_mem_top)
		fatal("%s: accessing device memory not allocated",
				__FUNCTION__);

	/* Write memory from host to device */
	buf = xmalloc(size);
	mem_read(si_emu->video_mem, src_ptr, size, buf);
	mem_write(si_emu->video_mem, dest_ptr, size, buf);
	free(buf);

	/* Return */
	return 0;
}


/*
 * OpenCL ABI call #6 - si_mem_free
 *
 * Deallocated memory in Southern Islands global memory scope.
 *
 * @param unsigned int device_ptr
 *
 * 	Memory address in device global memory returned previously by a call to
 *	'si_mem_alloc'.
 *
 * @return void
 *
 *	No value is returned.
 */

static int opencl_abi_si_mem_free_impl(struct x86_ctx_t *ctx)
{
	struct x86_regs_t *regs = ctx->regs;

	unsigned int device_ptr;

	/* Arguments */
	device_ptr = regs->ecx;
	opencl_debug("\tdevice_ptr = %u\n", device_ptr);

	/* For now, this call is ignored. No deallocation of global memory can
	 * happen. */

	/* Return device pointer */
	return device_ptr;
}



/*
 * OpenCL ABI call #7 - si_program_create
 *
 * Create a Southern Islands program object and return a unique identifier
 * for it.
 *
 * @return int
 *
 *	Unique program ID.
 */

static int opencl_abi_si_program_create_impl(struct x86_ctx_t *ctx)
{
	struct opencl_si_program_t *program;

	/* Create program */
	program = opencl_si_program_create();
	opencl_debug("\tnew program ID = %d\n", program->id);

	/* Return program ID */
	return program->id;
}




/*
 * OpenCL ABI call #8 - si_program_set_binary
 *
 * Associate a binary to a Southern Islands program.
 *
 * @param int program_id
 *
 * 	Program ID, as returned by a previous ABI call to 'si_program_create'.
 *
 * @param void *buf
 *
 * 	Pointer to the memory space where the program binary can be found.
 *
 * @param unsigned int size
 *
 * 	Size of the program binary
 *
 * @return void
 *
 *	No return value.
 */

static int opencl_abi_si_program_set_binary_impl(struct x86_ctx_t *ctx)
{
	struct x86_regs_t *regs = ctx->regs;
	struct opencl_si_program_t *program;

	int program_id;
	unsigned int bin_ptr;
	unsigned int bin_size;

	void *buf;

	/* Arguments */
	program_id = regs->ecx;
	bin_ptr = regs->edx;
	bin_size = regs->esi;
	opencl_debug("\tprogram_id=%d, bin_ptr=0x%x, size=%u\n",
			program_id, bin_ptr, bin_size);

	/* Get program */
	program = list_get(opencl_si_program_list, program_id);
	if (!program)
		fatal("%s: invalid program ID (%d)",
				__FUNCTION__, program_id);

	/* Set the binary */
	buf = xmalloc(bin_size);
	mem_read(ctx->mem, bin_ptr, bin_size, buf);
	opencl_si_program_set_binary(program, buf, bin_size);
	free(buf);

	/* No return value */
	return 0;
}



/*
 * OpenCL ABI call #9 - si_kernel_create
 *
 * Create a Southern Islands kernel object and return a unique identifier
 * for it.
 *
 * @param int program_id
 *
 * 	Program ID, as returned by ABI call 'si_program_create'
 *
 * @param char *func_name
 *
 * 	Kernel function name in the program.
 *
 * @return int
 *
 *	Unique kernel ID.
 */

static int opencl_abi_si_kernel_create_impl(struct x86_ctx_t *ctx)
{
	struct x86_regs_t *regs = ctx->regs;
	struct mem_t *mem = ctx->mem;

	struct opencl_si_kernel_t *kernel;
	struct opencl_si_program_t *program;

	unsigned int func_name_ptr;

	int program_id;
	int size;

	char func_name[MAX_STRING_SIZE];

	/* Arguments */
	program_id = regs->ecx;
	func_name_ptr = regs->edx;
	opencl_debug("\tprogram_id=%d, func_name_ptr=0x%x\n",
			program_id, func_name_ptr);

	/* Read function name */
	size = mem_read_string(mem, func_name_ptr, sizeof func_name, func_name);
	if (size == sizeof func_name)
		fatal("%s: buffer too small", __FUNCTION__);
	opencl_debug("\tfunc_name='%s'\n", func_name);

	/* Get program object */
	program = list_get(opencl_si_program_list, program_id);
	if (!program)
		fatal("%s: invalid program ID (%d)",
				__FUNCTION__, program_id);

	/* Create kernel */
	kernel = opencl_si_kernel_create(program, func_name);
	opencl_debug("\tnew kernel ID = %d\n", kernel->id);

	/* Return kernel ID */
	return kernel->id;
}




/*
 * OpenCL ABI call #10 - si_kernel_set_arg_value
 *
 * Set a kernel argument with a basic type (cl_char, cl_int, cl_float, ...).
 *
 * @param int kernel_id
 *
 * 	Kernel ID, as returned by ABI call 'si_kernel_create'
 *
 * @param int index
 *
 * 	Argument index to set.
 *
 * @param void *host_ptr
 *
 *	Address in host memory containing the value of the argument. The memory
 * 	pointed to by this variable will be copied internally, keeping a copy of
 * 	the argument for future use.
 *
 * @param unsigned int size
 *
 * 	Argument size. This size must match the size encoded in the kernel
 * 	metadata for this particular argument.
 *
 * @return int
 *
 *	Unique kernel ID.
 */

static int opencl_abi_si_kernel_set_arg_value_impl(struct x86_ctx_t *ctx)
{
	struct x86_regs_t *regs = ctx->regs;
	struct opencl_si_kernel_t *kernel;
	struct opencl_si_arg_t *arg;

	int kernel_id;

	unsigned int index;
	unsigned int host_ptr;
	unsigned int size;

	/* Arguments */
	kernel_id = regs->ecx;
	index = regs->edx;
	host_ptr = regs->esi;
	size = regs->edi;
	opencl_debug("\tkernel_id=%d, index=%d\n", kernel_id, index);
	opencl_debug("\thost_ptr=0x%x, size=%u\n", host_ptr, size);

	/* Get kernel */
	kernel = list_get(opencl_si_kernel_list, kernel_id);
	if (!kernel)
		fatal("%s: invalid kernel ID (%d)",
				__FUNCTION__, kernel_id);

	/* Get argument */
	arg = list_get(kernel->arg_list, index);
	if (!arg || arg->type != opencl_si_arg_value)
		fatal("%s: invalid argument %d type",
				__FUNCTION__, index);

	/* Check valid size */
	if (size != arg->size)
		fatal("%s: argument %d: size %d expected, %d found",
				__FUNCTION__, index, arg->size, size);

	/* Free a possible previous value */
	if (arg->value.value_ptr)
		free(arg->value.value_ptr);

	/* Save value */
	arg->set = 1;
	arg->value.value_ptr = xmalloc(size);
	mem_read(ctx->mem, host_ptr, size, arg->value.value_ptr);

	/* No return value */
	return 0;
}




/*
 * OpenCL ABI call #11 - si_kernel_set_arg_pointer
 *
 * Set a kernel argument of type 'cl_mem', or local memory. In general, any
 * argument that uses the 'pointer' name as first token in the metadata entry of
 * the kernel binary.
 *
 * @param int kernel_id
 *
 * 	Kernel ID, as returned by ABI call 'si_kernel_create'
 *
 * @param int index
 *
 * 	Argument index to set.
 *
 * @param void *device_ptr
 *
 *	If the argument represents a 'cl_mem' object in global memory, pointer
 *	to device memory containing the data, as returned by a previous call to
 *	'si_mem_alloc'.
 *	If the argument is a variable in local memory, the purpose of the call
 *	is just allocating space for it, so this value should be NULL.
 *
 * @param unsigned int size
 *
 *	If the argument represents a 'cl_mem' object, size allocated in global
 *	memory for the object.
 *	If the argument is a variable in local memory, number of bytes to be
 *	allocated in the device local memory.
 *
 * @return int
 *
 *	Unique kernel ID.
 */

static int opencl_abi_si_kernel_set_arg_pointer_impl(struct x86_ctx_t *ctx)
{
	struct x86_regs_t *regs = ctx->regs;
	struct opencl_si_kernel_t *kernel;
	struct opencl_si_arg_t *arg;

	int kernel_id;

	unsigned int index;
	unsigned int device_ptr;
	unsigned int size;

	/* Arguments */
	kernel_id = regs->ecx;
	index = regs->edx;
	device_ptr = regs->esi;
	size = regs->edi;
	opencl_debug("\tkernel_id=%d, index=%d\n", kernel_id, index);
	opencl_debug("\tdevice_ptr=0x%x, size=%u\n", device_ptr, size);

	/* Get kernel */
	kernel = list_get(opencl_si_kernel_list, kernel_id);
	if (!kernel)
		fatal("%s: invalid kernel ID (%d)",
				__FUNCTION__, kernel_id);

	/* Get argument */
	arg = list_get(kernel->arg_list, index);
	if (!arg || arg->type != opencl_si_arg_pointer)
		fatal("%s: invalid argument %d type",
				__FUNCTION__, index);

	/* Record size and value */
	arg->set = 1;
	arg->size = size;
	arg->pointer.device_ptr = device_ptr;

	/* No return value */
	return 0;
}




/*
 * OpenCL ABI call #12 - si_kernel_set_arg_image
 *
 * Set a kernel argument of type 'cl_image'.
 *
 * @param int kernel_id
 *
 * 	Kernel ID, as returned by ABI call 'si_kernel_create'
 *
 * @param int index
 *
 * 	Argument index to set.
 *
 * @param [...] FIXME - not decided yet
 *
 * @return int
 *
 *	Unique kernel ID.
 */

static int opencl_abi_si_kernel_set_arg_image_impl(struct x86_ctx_t *ctx)
{
	struct x86_regs_t *regs = ctx->regs;

	int kernel_id;
	unsigned int index;

	/* Arguments */
	kernel_id = regs->ecx;
	index = regs->edx;
	opencl_debug("\tkernel_id=%d, index=%d\n", kernel_id, index);

	/* Not yet */
	fatal("%s: not implemented", __FUNCTION__);

	/* No return value */
	return 0;
}




/*
 * OpenCL ABI call #13 - si_kernel_set_arg_sampler
 *
 * Set a kernel argument of type 'cl_sampler'.
 *
 * @param int kernel_id
 *
 * 	Kernel ID, as returned by ABI call 'si_kernel_create'
 *
 * @param int index
 *
 * 	Argument index to set.
 *
 * @param [...] FIXME - not decided yet
 *
 * @return int
 *
 *	Unique kernel ID.
 */

static int opencl_abi_si_kernel_set_arg_sampler_impl(struct x86_ctx_t *ctx)
{
	struct x86_regs_t *regs = ctx->regs;

	int kernel_id;
	unsigned int index;

	/* Arguments */
	kernel_id = regs->ecx;
	index = regs->edx;
	opencl_debug("\tkernel_id=%d, index=%d\n", kernel_id, index);

	/* Not yet */
	fatal("%s: not implemented", __FUNCTION__);

	/* No return value */
	return 0;
}




/*
 * OpenCL ABI call #14 - si_ndrange_initialize
 *
 * Create and initialize an ND-Range for the supplied kernel.
 *
 * @param int kernel_id
 *
 * 	Kernel ID, as returned by ABI call 'si_kernel_create'
 *
 * @param int work_dim
 *
 * 	Number of work dimensions. This is an integer number between 1 and 3,
 * 	which determines the number of elements of the following arrays.
 *
 * @param unsigned int *global_offset
 *
 *	Array of 'work_dim' integers containing global offsets.
 *
 * @param unsigned int *global_size
 *
 *	Array of 'work_dim' integers containing the ND-Range global size in each
 *	dimension.
 *
 * @param unsigned int *local_size
 *
 *	Array of 'work_dim' integers containing the local size in each
 *	dimension.
 *
 * @return int
 *
 *	Unique kernel ID.
 */

static int opencl_abi_si_ndrange_initialize_impl(struct x86_ctx_t *ctx)
{
	struct elf_buffer_t *elf_buffer;
	struct mem_t *mem = ctx->mem;
	struct opencl_si_kernel_t *kernel;
	struct si_bin_enc_user_element_t *user_elements;
	struct si_ndrange_t *ndrange;
	struct x86_regs_t *regs = ctx->regs;

	int i;
	int kernel_id;
	int user_element_count;
	int work_dim;

	unsigned int global_offset_ptr;
	unsigned int global_size_ptr;
	unsigned int local_size_ptr;

	unsigned int global_offset[3];
	unsigned int global_size[3];
	unsigned int local_size[3];

	if (fused_device)
		fatal("Fused device not supported\n");  // FIXME

	if (fused_device)
		si_emu->global_mem = ctx->mem;

	/* Arguments */
	kernel_id = regs->ecx;
	work_dim = regs->edx;
	global_offset_ptr = regs->esi;
	global_size_ptr = regs->edi;
	local_size_ptr = regs->ebp;
	opencl_debug("\tkernel_id=%d, work_dim=%d\n", kernel_id, work_dim);
	opencl_debug("\tglobal_offset_ptr=0x%x, global_size_ptr=0x%x, "
		"local_size_ptr=0x%x\n", global_offset_ptr, global_size_ptr, 
		local_size_ptr);
	
	/* Debug */
	assert(IN_RANGE(work_dim, 1, 3));
	mem_read(mem, global_offset_ptr, work_dim * 4, global_offset);
	mem_read(mem, global_size_ptr, work_dim * 4, global_size);
	mem_read(mem, local_size_ptr, work_dim * 4, local_size);
	for (i = 0; i < work_dim; i++)
		opencl_debug("\tglobal_offset[%d] = %u\n", i, global_offset[i]);
	for (i = 0; i < work_dim; i++)
		opencl_debug("\tglobal_size[%d] = %u\n", i, global_size[i]);
	for (i = 0; i < work_dim; i++)
		opencl_debug("\tlocal_size[%d] = %u\n", i, local_size[i]);

	/* Get kernel */
	kernel = list_get(opencl_si_kernel_list, kernel_id);
	if (!kernel)
		fatal("%s: invalid kernel ID (%d)", __FUNCTION__, kernel_id);

	/* Create ND-Range */
	ndrange = si_ndrange_create();
	ndrange->id = si_emu->ndrange_count++;
	ndrange->local_mem_top = kernel->mem_size_local;
	ndrange->num_sgpr_used = kernel->bin_file->
		enc_dict_entry_southern_islands->num_sgpr_used;
	ndrange->num_vgpr_used = kernel->bin_file->
		enc_dict_entry_southern_islands->num_vgpr_used;
	ndrange->wg_id_sgpr = kernel->bin_file->
		enc_dict_entry_southern_islands->compute_pgm_rsrc2->user_sgpr;
	si_ndrange_setup_size(ndrange, global_size, local_size, work_dim);

	/* Copy user elements from kernel to ND-Range */
	user_element_count = kernel->bin_file->
		enc_dict_entry_southern_islands->userElementCount;
	user_elements = kernel->bin_file->enc_dict_entry_southern_islands->
		userElements;
	ndrange->userElementCount = user_element_count;
	for (i = 0; i < user_element_count; i++)
	{
		ndrange->userElements[i] = user_elements[i];
	}

	/* Set up instruction memory */
	/* Initialize wavefront instruction buffer and PC */
	elf_buffer = &kernel->bin_file->enc_dict_entry_southern_islands->
		sec_text_buffer;
	if (!elf_buffer->size)
		fatal("%s: cannot load kernel code", __FUNCTION__);

	si_ndrange_setup_inst_mem(ndrange, elf_buffer->ptr, 
		elf_buffer->size, 0);

	assert(!driver_state.kernel);
	driver_state.kernel = kernel;

	assert(!driver_state.ndrange);
	driver_state.ndrange = ndrange;

	assert(!si_emu->ndrange);
	si_emu->ndrange = ndrange;

	if (si_gpu)
		si_gpu_map_ndrange(ndrange);

	/* No return value */
	return 0;
}




/*
 * OpenCL ABI call #15 - si_ndrange_get_num_buffer_entries
 *
 * Returns the number of available buffer entries in the waiting 
 * work-group queue.
 *
 * @param unsigned int *host_ptr
 *
 *	Location to be populated with the number of available 
 *	buffer entry slots.
 *
 * @return int
 *
 *	The function always returns 0.
 */


static int opencl_abi_si_ndrange_get_num_buffer_entries_impl(
	struct x86_ctx_t *ctx)
{
	struct x86_regs_t *regs = ctx->regs;
	struct mem_t *mem = ctx->mem;

	unsigned int host_ptr;

	int available_buffer_entries;

	/* Arguments */
	host_ptr = regs->ecx;

	available_buffer_entries = SI_DRIVER_MAX_WORK_GROUP_BUFFER_SIZE -
		list_count(si_emu->waiting_work_groups);

	mem_write(mem, host_ptr, sizeof available_buffer_entries,
		&available_buffer_entries);

	return 0;
}

/*
 * OpenCL ABI call #16 - si_ndrange_send_work_groups
 *
 * Receives a range of work-group IDs to add to the waiting 
 * work-group queue. The x86 context performing this call
 * suspends until the emulator needs more work.
 *
 * @param int work_group_start_id 
 *
 *	The first work-group ID to add to the waiting queue.
 *
 * @param int work_group_count
 *
 *	The number of work groups to add to the waiting queue.
 *
 * @return int
 *
 *	The function always returns 0.
 */

static int opencl_abi_si_ndrange_send_work_groups_can_wakeup(
	struct x86_ctx_t *ctx, void *user_data)
{
	assert(!user_data);

	return driver_state.ready_for_work;
}

static void opencl_abi_si_ndrange_send_work_groups_wakeup(
	struct x86_ctx_t *ctx, void *user_data)
{
	assert(!user_data);
}

static int opencl_abi_si_ndrange_send_work_groups_impl(struct x86_ctx_t *ctx)
{
	struct x86_regs_t *regs = ctx->regs;

	int i;
	int work_group_count;
	int work_group_start_id;

	long work_group_id;

	/* Arguments */
	work_group_start_id = regs->ecx;
	work_group_count = regs->edx;

	assert(work_group_count <= SI_DRIVER_MAX_WORK_GROUP_BUFFER_SIZE -
		list_count(si_emu->waiting_work_groups));

	opencl_debug("    receiving work groups %d through %d\n",
		work_group_start_id, work_group_start_id+work_group_count-1);

	/* Receive work groups (add them to the waiting queue) */
	for (i = 0; i < work_group_count; i++)
	{
		work_group_id = work_group_start_id + i;
		list_enqueue(si_emu->waiting_work_groups, (void*)work_group_id);
	}

	/* XXX Later, check if waiting queue is still under a threshold.  
	 * If it is, set as ready for work */
	driver_state.ready_for_work = 0;

	/* Suspend x86 context until driver needs more work */
	x86_ctx_suspend(ctx, 
		opencl_abi_si_ndrange_send_work_groups_can_wakeup, NULL,
		opencl_abi_si_ndrange_send_work_groups_wakeup, NULL);

	return 0;
}

/*
 * OpenCL ABI call #17 - si_ndrange_finish
 *
 * Tells the driver that there are no more work groups to execute
 * from the ND-Range.
 *
 * @return int
 *
 *	The function always returns 0.
 */

static int opencl_abi_si_ndrange_finish_can_wakeup(struct x86_ctx_t *ctx, 
	void *user_data)
{
	assert(!user_data);

	return driver_state.ndrange_complete;
}

static void opencl_abi_si_ndrange_finish_wakeup(struct x86_ctx_t *ctx, 
	void *user_data)
{
	assert(!user_data);

	/* Reset driver state */
	si_ndrange_free(driver_state.ndrange);
	driver_state.ndrange = NULL;
	driver_state.kernel = NULL;
	driver_state.wait_for_ndrange_completion = 0;
	driver_state.ndrange_complete = 0;
	driver_state.ready_for_work = 0;

	si_emu->ndrange = NULL;
}

static int opencl_abi_si_ndrange_finish_impl(struct x86_ctx_t *ctx)
{
	driver_state.wait_for_ndrange_completion = 1;

	if (!list_count(si_emu->running_work_groups) && 
		!list_count(si_emu->waiting_work_groups))
	{
		/* Reset driver state */
		si_ndrange_free(driver_state.ndrange);
		driver_state.ndrange = NULL;
		driver_state.kernel = NULL;
		driver_state.wait_for_ndrange_completion = 0;
		driver_state.ndrange_complete = 0;
		driver_state.ready_for_work = 0;

		si_emu->ndrange = NULL;
	}
	else 
	{
		/* Suspend x86 context until simulation completes */
		x86_ctx_suspend(ctx, opencl_abi_si_ndrange_finish_can_wakeup, 
			NULL, opencl_abi_si_ndrange_finish_wakeup, NULL);
	}

	return 0;
}


/*
 * OpenCL ABI call #18 - si_ndrange_pass_mem_objs
 *
 * @return int
 *
 *	The function always returns 0.
 */

static int opencl_abi_si_ndrange_pass_mem_objs_impl(struct x86_ctx_t *ctx)
{
	struct opencl_si_kernel_t *kernel;
	struct si_ndrange_t *ndrange;
	struct x86_regs_t *regs = ctx->regs;

	unsigned int tables_ptr;
	unsigned int constant_buffers_ptr;

	if (fused_device)
	{
		/* Arguments */
		tables_ptr = regs->ecx;
		constant_buffers_ptr = regs->edx;

		si_emu->ndrange->const_buf_table = tables_ptr;

		/* The successive tables must be aligned */
		si_emu->ndrange->resource_table = 
			(si_emu->ndrange->const_buf_table + 
			SI_EMU_CONST_BUF_TABLE_SIZE + 16) & 0xFFFFFFF0;
		
		si_emu->ndrange->uav_table = 
			(si_emu->ndrange->resource_table +
			SI_EMU_RESOURCE_TABLE_SIZE + 16) & 0xFFFFFFF0;

		si_emu->ndrange->cb0 = constant_buffers_ptr;
		si_emu->ndrange->cb1 = si_emu->ndrange->cb0 + 
			SI_EMU_CONST_BUF_0_SIZE;
	}

	kernel = driver_state.kernel;
	ndrange = driver_state.ndrange;

	/* Set up initial state and arguments (order matters!) */
	if (!fused_device)
	{
		opencl_si_kernel_create_ndrange_tables(ndrange); 
		opencl_si_kernel_create_ndrange_constant_buffers(ndrange); 
	}
	opencl_si_kernel_setup_ndrange_constant_buffers(ndrange);
	opencl_si_kernel_setup_ndrange_args(kernel, ndrange);
	opencl_si_kernel_debug_ndrange_state(kernel, ndrange);

	return 0;
}

/*
 *  Helper functions
 */

void opencl_si_request_work()
{
	if (driver_state.wait_for_ndrange_completion && 
		!list_count(si_emu->running_work_groups) && 
		!list_count(si_emu->waiting_work_groups))
	{
		opencl_debug("ND-Range is complete\n");
		driver_state.ndrange_complete = 1;
	}
	else 
	{
		opencl_debug("SI is ready for more work\n");
		driver_state.ready_for_work = 1;
	}

	x86_emu_process_events_schedule();
}
