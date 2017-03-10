/*
 * Copyright (C) 2015-2017 University of Stuttgart
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
//#include <dirent.h>
//#include <ctype.h>
#include <hwloc.h>
#include <papi.h>

#include "mf_RAPL_power_connector.h"

#define SUCCESS 1
#define FAILURE 0

/*******************************************************************************
 * Variable Declarations
 ******************************************************************************/
/* time in seconds */
double before_time, after_time; 

int EventSet = PAPI_NULL;
int num_sockets = 0;
double denominator = 1.0 ; /*according to different CPU models, DRAM energy scalings are different */
int rapl_is_available = 0;
float epackage_before[4], edram_before[4], epackage_after[4], edram_after[4]; //max sockets number is 4

/*******************************************************************************
 * Forward Declarations
 ******************************************************************************/
int events_are_all_not_valid(char **events, size_t num_events);
int rapl_init(Plugin_metrics *data);
int load_papi_library(void);
int check_rapl_component(void);
int hardware_sockets_count(void);
double rapl_get_denominator(void);
void native_cpuid(unsigned int *eax, unsigned int *ebx, unsigned int *ecx, unsigned int *edx);
int rapl_stat_read(float *epackage, float *edram);


int mf_RAPL_power_init(Plugin_metrics *data, char **events, size_t num_events)
{
	if (events_are_all_not_valid(events, num_events)) {
		return FAILURE;
	}

	rapl_is_available = rapl_init(data);
	if(rapl_is_available == 0 || data->num_events == 0) {
		return FAILURE;
	}
	rapl_stat_read(epackage_before, edram_before);

	/* get the before timestamp in second */
	struct timespec timestamp;
	clock_gettime(CLOCK_REALTIME, &timestamp);
    before_time = timestamp.tv_sec * 1.0  + (double)(timestamp.tv_nsec / 1.0e9);

    return SUCCESS;
}

int mf_RAPL_power_sample(Plugin_metrics *data)
{
	/* get current timestamp in second */
	struct timespec timestamp;
	clock_gettime(CLOCK_REALTIME, &timestamp);
    after_time = timestamp.tv_sec * 1.0  + (double)(timestamp.tv_nsec / 1.0e9);

	double time_interval = after_time - before_time; /* get time interval */

	if(rapl_is_available) {
		rapl_stat_read(epackage_after, edram_after);	
	}
	int i, ii = 0;
	for(i = 0; i < num_sockets; i++) {
		if(strstr(data->events[ii], "total_power") != NULL) {
			data->values[ii] = (epackage_after[i] - epackage_before[i]) / time_interval;	//unit is milliWatt
			ii++;
			epackage_before[i] = epackage_after[i];
		}
		if(strstr(data->events[ii], "dram_power") != NULL) {
			data->values[ii] = (edram_after[i] - edram_before[i]) / time_interval;		//unit is milliWatt
			ii++;
			edram_before[i] = edram_after[i];
		}
	}

	/* update timestamp */
	before_time = after_time;
	return SUCCESS;
}

void mf_RAPL_power_to_json(Plugin_metrics *data, char *json)
{
    char tmp[128] = {'\0'};
    int i;
    /*
     * prepares the json string, including current timestamp, and name of the plugin
     */
    sprintf(json, "\"type\":\"RAPL_power\"");
    sprintf(tmp, ",\"local_timestamp\":\"%.1f\"", after_time * 1.0e3);
    strcat(json, tmp);

    /*
     * filters the sampled data with respect to metrics values
     */
	for (i = 0; i < data->num_events; i++) {
		/* if metrics' value >= 0.0, append the metrics to the json string */
		if(data->values[i] >= 0.0) {
			sprintf(tmp, ",\"%s\":%.3f", data->events[i], data->values[i]);
			strcat(json, tmp);
		}
	}
}

int events_are_all_not_valid(char **events, size_t num_events) 
{
	int i, counter;
	counter = 0; 
	for (i = 0; i < num_events; i++) {
		/* if events name matches, counter is incremented by 1 */
		if(strcmp(events[i], "total_power") == 0) {
			counter++;
		}
		if(strcmp(events[i], "dram_power") == 0) {
			counter++;
		}

	}
	if (counter == 0) {
		fprintf(stderr, "Wrong given metrics.\nPlease given metrics total_power, dram_power.\n");
		return 1;
	}
	else {
		return 0;
	}
}

/* initialize RAPL counters prepare eventset and start counters */
int rapl_init(Plugin_metrics *data) 
{
	/* Load PAPI library */
	if (!load_papi_library()) {
        return FAILURE;
    }

    /* check if rapl component is enabled */
    if (!check_rapl_component()) {
        return FAILURE;
    }

    /* get the number of sockets */
    num_sockets = hardware_sockets_count();
    if(num_sockets <= 0) {
    	return FAILURE;
    }

	/* creat an PAPI EventSet */
	if (PAPI_create_eventset(&EventSet) != PAPI_OK) {
		fprintf(stderr, "Error: PAPI_create_eventset failed.\n");
		return FAILURE;
	}

	/* add for each socket the package energy and dram energy events */
	int i, ii, ret;
	ii = 0;
	char event_name[32] = {'\0'};

	for (i = 0; i < num_sockets; i++) {
		memset(event_name, '\0', 32 * sizeof(char));
		sprintf(event_name, "PACKAGE_ENERGY:PACKAGE%d", i);
		ret = PAPI_add_named_event(EventSet, event_name);
		if (ret == PAPI_OK) {
			data->events[ii] = malloc(MAX_EVENTS_LEN * sizeof(char));	
    		sprintf(data->events[ii], "package[%d]:total_power", i);
    		ii++;
		}
		
		memset(event_name, '\0', 32 * sizeof(char));
		sprintf(event_name, "DRAM_ENERGY:PACKAGE%d", i);
		ret = PAPI_add_named_event(EventSet, event_name);
		if (ret == PAPI_OK) {
			data->events[ii] = malloc(MAX_EVENTS_LEN * sizeof(char));	
    		sprintf(data->events[ii], "package[%d]:dram_power", i);
    		ii++;
		}
	}

	data->num_events = ii;
	/* set dominator for DRAM energy values based on different CPU model */
	denominator = rapl_get_denominator();

	if (PAPI_start(EventSet) != PAPI_OK) {
		fprintf(stderr, "PAPI_start failed.\n");
		return FAILURE;
	}

	return SUCCESS;

}

/* Load the PAPI library */
int load_papi_library(void)
{
    if (PAPI_is_initialized()) {
        return SUCCESS;
    }

    int ret = PAPI_library_init(PAPI_VER_CURRENT);
    if (ret != PAPI_VER_CURRENT) {
        char *error = PAPI_strerror(ret);
        fprintf(stderr, "Error while loading the PAPI library: %s\n", error);
        return FAILURE;
    }

    return SUCCESS;
}

/* Check if rapl component is enabled */
int check_rapl_component(void)
{
	int numcmp, cid; /* number of component and component id variables declare */
	const PAPI_component_info_t *cmpinfo = NULL;

	numcmp = PAPI_num_components();
    for (cid = 0; cid < numcmp; cid++) {
        cmpinfo = PAPI_get_component_info(cid);
        if (strstr(cmpinfo->name, "rapl")) {
            if (cmpinfo->disabled) {
                fprintf(stderr, "Component RAPL is DISABLED\n");
                return FAILURE;
            } else {
                return SUCCESS;
            }
        }
    }
    return FAILURE;
}

/* Count the number of available sockets by hwloc library. 
 * return the number of sockets on success; 0 otherwise
 */
int hardware_sockets_count(void)
{
	int depth;
	int skts_num = 0;
	hwloc_topology_t topology;
	hwloc_topology_init(&topology);
	hwloc_topology_load(topology);
	depth = hwloc_get_type_depth(topology, HWLOC_OBJ_SOCKET);
	if (depth == HWLOC_TYPE_DEPTH_UNKNOWN) {
		fprintf(stderr, "Error: The number of sockets is unknown.\n");
		return 0;
	}
	skts_num = hwloc_get_nbobjs_by_depth(topology, depth);
	return skts_num;
}

/*get the coefficient of current CPU model */
double rapl_get_denominator(void)
{
	/* get cpu model */
    unsigned int eax, ebx, ecx, edx;
    eax = 1;
    native_cpuid(&eax, &ebx, &ecx, &edx);
    int cpu_model = (eax >> 4) & 0xF;

    if (cpu_model == 15) {
        return 15.3;
    } else {
        return 1.0;
    }

}

/* Get native cpuid */
void native_cpuid(unsigned int *eax, unsigned int *ebx, unsigned int *ecx, unsigned int *edx)
{
    asm volatile("cpuid"
        : "=a" (*eax),
          "=b" (*ebx),
          "=c" (*ecx),
          "=d" (*edx)
        : "0" (*eax), "2" (*ecx)
    );
}

/* Read rapl counters values, computer the energy values for CPU and DRAM; (in milliJoule)
   counters are reset after read */
int rapl_stat_read(float *epackage, float *edram) 
{
	int i, ret;
	long long *values = malloc(2 * num_sockets * sizeof(long long));

	ret = PAPI_read(EventSet, values);
	if(ret != PAPI_OK) {
		char *error = PAPI_strerror(ret);
		fprintf(stderr, "Error while reading the PAPI counters: %s\n", error);
        return FAILURE;
	}
	
	for(i = 0; i < num_sockets * 2; i++) {
		epackage[i] = (float) (values[i] * 1.0e-6);
		i++;
		edram[i] = (float) (values[i] * 1.0e-6 ) / denominator;
	}
	PAPI_reset(EventSet);
	
	return SUCCESS;
}