/*
 *  Multi2Sim
 *  Copyright (C) 2012  Yifan Sun (yifansun@coe.neu.edu)
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

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>

#include "debug.h"
#include "hsa.h"

struct hsa_runtime_t *hsa_runtime;

hsa_status_t HSA_API hsa_init()
{
	if (hsa_runtime == NULL)
	{
		hsa_runtime = calloc(1, sizeof(struct hsa_runtime_t));
		hsa_runtime->fd = open("/dev/hsa", O_RDWR);
		if (hsa_runtime->fd < 0)
			fatal("Cannot communicate with the HSA driver\n\n"
				"This error could be due to an incompatibility between the\n"
				"Multi2Sim HSA driver version and the version of the simulator.\n"
				"Please download the latest versions and retry.");

		// Call the driver init hsa environment
		ioctl(hsa_runtime->fd, InitFromX86, getpid());

		// Return success
		return HSA_STATUS_SUCCESS;
	}
	else
	{
		// Success silently
		return HSA_STATUS_SUCCESS;
	}
}


hsa_status_t HSA_API hsa_shut_down()
{
	return HSA_STATUS_SUCCESS;
}


hsa_status_t HSA_API hsa_system_get_info(
		hsa_system_info_t attribute, void *value)
{
	unsigned int args[3];
	args[1] = (unsigned int)attribute;
	args[2] = (unsigned int)value;

	if (!hsa_runtime)
	{
		return HSA_STATUS_ERROR_NOT_INITIALIZED;
	}
	else
	{
		ioctl(hsa_runtime->fd, SystemGetInfo, args);
		return (hsa_status_t)args[0];
	}
}


hsa_status_t HSA_API hsa_iterate_agents(
		hsa_status_t (*callback)(hsa_agent_t agent, void *data),
		void *data)
{
	unsigned long long agent_ids[2];
	agent_ids[0] = 1;
	agent_ids[1] = 0;

	// Traverse all agent
	while(agent_ids[0] != 0)
	{
		ioctl(hsa_runtime->fd, NextAgent, agent_ids);
		if (agent_ids[0] != 0)
		{
			callback(agent_ids[0], data);
			agent_ids[1] = agent_ids[0];
		}
	}
	return HSA_STATUS_SUCCESS;
}


hsa_status_t HSA_API hsa_agent_get_info(hsa_agent_t agent,
                                        hsa_agent_info_t attribute,
                                        void *value)
{
	char args[4 + 8 + 4 + 4];
	memcpy(args + 4, &agent, 8);
	memcpy(args + 12, &attribute, 4);
	memcpy(args + 16, &value, 4);

	if (!hsa_runtime)
	{
		return HSA_STATUS_ERROR_NOT_INITIALIZED;
	}
	else
	{
		ioctl(hsa_runtime->fd, AgentGetInfo, args);
		return (hsa_status_t)args[0];
	}
}


hsa_status_t HSA_API hsa_queue_create(hsa_agent_t agent,
		size_t size, hsa_queue_type_t type,
		void (*callback)(hsa_status_t status, hsa_queue_t *queue),
		const hsa_queue_t *service_queue, hsa_queue_t **queue)
{
	// Set arguments
	char args[56] = {0};
	memcpy(args + 4, &agent, 8);
	memcpy(args + 12, &size, 4);
	memcpy(args + 16, &type, 4);
	memcpy(args + 20, &callback, 4);
	//printf("callback %d\n", (unsigned)callback);
	memcpy(args + 28, &service_queue, 4);
	memcpy(args + 36, &queue, 4);
	unsigned int host_lang = 1;
	memcpy(args+44, &host_lang, 4);

	if (!hsa_runtime)
	{
		return HSA_STATUS_ERROR_NOT_INITIALIZED;
	}
	else
	{
		ioctl(hsa_runtime->fd, QueueCreate, args);
		return (hsa_status_t)args[0];
	}
}


uint64_t HSA_API hsa_queue_load_read_index_relaxed(hsa_queue_t *queue)
{
	unsigned long long args[2] = {0};
	memcpy(args + 1, &queue, 4);
	ioctl(hsa_runtime->fd, QueueLoadReadIndexRelaxed, args);
	return args[0];
}


uint64_t HSA_API hsa_queue_load_write_index_relaxed(hsa_queue_t *queue)
{
	unsigned long long args[2] = {0};
	memcpy(args + 1, &queue, 4);
	ioctl(hsa_runtime->fd, QueueLoadWriteIndexRelaxed, args);
	return args[0];
}


uint64_t HSA_API hsa_queue_add_write_index_relaxed(
		hsa_queue_t *queue, uint64_t value)
{
	unsigned long long args[3] = {0};
	memcpy(args + 1, &queue, 4);
	memcpy(args + 2, &value, 8);
	ioctl(hsa_runtime->fd, QueueAddWriteIndexRelaxed, args);
	return args[0];
}


hsa_status_t HSA_API hsa_ext_program_create(
	hsa_machine_model_t machine_model,
	hsa_profile_t profile,
	hsa_default_float_rounding_mode_t default_float_rounding_mode,
	const char *options,
	hsa_ext_program_t *program)
{
	fatal("Runtime function %s not implemented", __FUNCTION__);
	return HSA_STATUS_SUCCESS;
}

hsa_status_t HSA_API hsa_ext_program_add_module(
	hsa_ext_program_t program,
	hsa_ext_module_t module)
{
	fatal("Runtime function %s not implemented", __FUNCTION__);
	return HSA_STATUS_SUCCESS;
}

hsa_status_t HSA_API hsa_ext_program_finalize(
	hsa_ext_program_t program,
	hsa_isa_t isa,
	int32_t call_convention,
	hsa_ext_control_directives_t control_directives,
	const char *options,
	hsa_code_object_type_t code_object_type,
	hsa_code_object_t *code_object )
{
	fatal("Runtime function %s not implemented", __FUNCTION__);
	return HSA_STATUS_SUCCESS;
}

hsa_status_t HSA_API
    hsa_executable_get_symbol(hsa_executable_t executable,
                              const char *module_name,
                              const char *symbol_name,
                              hsa_agent_t agent,
                              int32_t call_convention,
                              hsa_executable_symbol_t *symbol)
{
	fatal("Runtime function %s not implemented", __FUNCTION__);
	return HSA_STATUS_SUCCESS;
}

hsa_status_t HSA_API hsa_code_object_get_symbol(
	hsa_code_object_t code_object,
	const char *symbol_name,
	hsa_code_symbol_t *symbol)
{
	fatal("Runtime function %s not implemented", __FUNCTION__);
	return HSA_STATUS_SUCCESS;
}

hsa_status_t HSA_API
    hsa_executable_create(hsa_profile_t profile,
		          hsa_executable_state_t executable_stat,
		          const char *options,
		          hsa_executable_t *executable)
{
	fatal("Runtime function %s not implemented", __FUNCTION__);
	return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_executable_load_code_object(
	hsa_executable_t executable,
	hsa_agent_t agent,
	hsa_code_object_t code_object,
	const char *options)
{
	fatal("Runtime function %s not implemented", __FUNCTION__);
	return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_executable_freeze(
	hsa_executable_t executable,
	const char *options)
{
	fatal("Runtime function %s not implemented", __FUNCTION__);
	return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_executable_symbol_get_info(
	hsa_executable_symbol_t executable_symbol,
	hsa_executable_symbol_info_t attribute,
	void *value)
{
	fatal("Runtime function %s not implemented", __FUNCTION__);
	return HSA_STATUS_SUCCESS;
}


