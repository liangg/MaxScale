/*
 * This file is distributed as part of the MariaDB Corporation MaxScale.  It is free
 * software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation,
 * version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright MariaDB Corporation Ab 2013-2014
 */

/**
 * @file config.c  - Read the gateway.cnf configuration file
 *
 * @verbatim
 * Revision History
 *
 * Date		Who			Description
 * 21/06/13	Mark Riddoch		Initial implementation
 * 08/07/13	Mark Riddoch		Addition on monitor module support
 * 23/07/13	Mark Riddoch		Addition on default monitor password
 * 06/02/14	Massimiliano Pinto	Added support for enable/disable root user in services
 * 14/02/14	Massimiliano Pinto	Added enable_root_user in the service_params list
 * 11/03/14	Massimiliano Pinto	Added Unix socket support
 * 11/05/14	Massimiliano Pinto	Added version_string support to service
 * 19/05/14	Mark Riddoch		Added unique names from section headers
 * 29/05/14	Mark Riddoch		Addition of filter definition
 * 23/05/14	Massimiliano Pinto	Added automatic set of maxscale-id: first listening ipv4_raw + port + pid
 * 28/05/14	Massimiliano Pinto	Added detect_replication_lag parameter
 * 28/08/14	Massimiliano Pinto	Added detect_stale_master parameter
 * 09/09/14	Massimiliano Pinto	Added localhost_match_wildcard_host parameter
 * 12/09/14	Mark Riddoch		Addition of checks on servers list and
 *					internal router suppression of messages
 * 30/10/14	Massimiliano Pinto	Added disable_master_failback parameter
 * 07/11/14	Massimiliano Pinto	Addition of monitor timeouts for connect/read/write
 * 20/02/15	Markus Mäkelä		Added connection_timeout parameter for services
 * 05/03/15	Massimiliano Pinto	Added notification_feedback support
 * 20/04/15	Guillaume Lefranc	Added available_when_donor parameter
 * 22/04/15 Martin Brampton     Added disable_master_role_setting parameter
 * 22/04/15     Martin Brampton         Added disable_master_role_setting parameter
 * 19/05/15     Massimiliano Pinto	Code cleanup for process_config_update()
 * 20/05/15     Massimiliano Pinto	Addition of monitor update routines
 *
 * @endverbatim
 */
#include <my_config.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <ini.h>
#include <maxconfig.h>
#include <service.h>
#include <server.h>
#include <users.h>
#include <monitor.h>
#include <modules.h>
#include <skygw_utils.h>
#include <log_manager.h>
#include <mysql.h>
#include <sys/utsname.h>
#include <sys/fcntl.h>
#include <glob.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <housekeeper.h>
#include <notification.h>
#include <unistd.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/utsname.h>
#include <gwdirs.h>

#ifdef SS_DEBUG
#include <gwdirs.h>
#endif

/** Defined in log_manager.cc */
extern int            lm_enabled_logfiles_bitmask;
extern size_t         log_ses_count[];
extern __thread log_info_t tls_log_info;

extern int setipaddress(struct in_addr *, char *);
static	int	process_config_context(CONFIG_CONTEXT	*);
static	int	process_config_update(CONFIG_CONTEXT *);
static	void	free_config_context(CONFIG_CONTEXT	*);
static	char 	*config_get_value(CONFIG_PARAMETER *, const char *);
static	const char 	*config_get_value_string(CONFIG_PARAMETER *, const char *);
static	int	handle_global_item(const char *, const char *);
static	int	handle_feedback_item(const char *, const char *);
static	void	global_defaults();
static	void	feedback_defaults();
static	void	check_config_objects(CONFIG_CONTEXT *context);
int	config_truth_value(char *str);
static	int	internalService(char *router);
int	config_get_ifaddr(unsigned char *output);
int	config_get_release_string(char* release);
FEEDBACK_CONF * config_get_feedback_data();
void	config_add_param(CONFIG_CONTEXT*,char*,char*);
void	config_service_update(CONFIG_CONTEXT *obj);
void	config_server_update(CONFIG_CONTEXT *obj);
void	config_listener_update(CONFIG_CONTEXT *obj, CONFIG_CONTEXT *context);
void	config_service_update_objects(CONFIG_CONTEXT *obj, CONFIG_CONTEXT *context);
void	config_monitor_update(CONFIG_CONTEXT *obj, CONFIG_CONTEXT *context);
int	config_add_monitor(CONFIG_CONTEXT *obj, CONFIG_CONTEXT *context, MONITOR *running);
int config_add_filter(CONFIG_CONTEXT* obj);
void service_debug_show(char* service_name);
bool config_convert_integer(char* value, int* iptr);
bool config_verify_context();
static	char		*config_file = NULL;
static	GATEWAY_CONF	gateway;
static	FEEDBACK_CONF	feedback;
char			*version_string = NULL;
static HASHTABLE	*monitorhash;
static int		reload_state = RELOAD_INACTIVE;
static SPINLOCK         reload_lock = SPINLOCK_INIT;
/**
 * Trim whitespace from the front and rear of a string
 *
 * @param str		String to trim
 * @return	Trimmed string, changes are done in situ
 */
static char *
trim(char *str)
{
char	*ptr;

	while (isspace(*str))
		str++;

	/* Point to last character of the string */
	ptr = str + strlen(str) - 1;
	while (ptr > str && isspace(*ptr))
		*ptr-- = 0;

	return str;
}

/**
 * Config item handler for the ini file reader
 *
 * @param userdata	The config context element
 * @param section	The config file section
 * @param name		The Parameter name
 * @param value		The Parameter value
 * @return zero on error
 */
static int
handler(void *userdata, const char *section, const char *name, const char *value)
{
CONFIG_CONTEXT		*cntxt = (CONFIG_CONTEXT *)userdata;
CONFIG_CONTEXT		*ptr = cntxt;
CONFIG_PARAMETER	*param, *p1;

	if (strcmp(section, "gateway") == 0 || strcasecmp(section, "MaxScale") == 0)
	{
		return handle_global_item(name, value);
	}

	if (strcasecmp(section, "feedback") == 0)
	{
		return handle_feedback_item(name, value);
	}

	/*
	 * If we already have some parameters for the object
	 * add the parameters to that object. If not create
	 * a new object.
	 */
	while (ptr && strcmp(ptr->object, section) != 0)
		ptr = ptr->next;
	if (!ptr)
	{
		if ((ptr = (CONFIG_CONTEXT *)malloc(sizeof(CONFIG_CONTEXT))) == NULL)
			return 0;
		ptr->object = strdup(section);
		ptr->parameters = NULL;
		ptr->next = cntxt->next;
		ptr->element = NULL;
		cntxt->next = ptr;
	}
	/* Check to see if the parameter already exists for the section */
	p1 = ptr->parameters;
	while (p1)
	{
		if (!strcmp(p1->name, name))
		{
			LOGIF(LE, (mxs_log_flush(
                                LOGFILE_ERROR,
                                "Error : Configuration object '%s' has multiple "
				"parameters named '%s'.",
                                ptr->object, name)));
			return 0;
		}
		p1 = p1->next;
	}


	if ((param = (CONFIG_PARAMETER *)malloc(sizeof(CONFIG_PARAMETER))) == NULL)
		return 0;
	param->name = strdup(name);
	param->value = strdup(value);
	param->next = ptr->parameters;
	param->qfd_param_type = UNDEFINED_TYPE;
	ptr->parameters = param;

	return 1;
}

/**
 * Handle SSL parameters and configure the service to use SSL.
 * @param service Service to configure
 * @param mode SSL mode, one of 'enabled', 'required' and 'disabled'
 * @param ssl_cert SSL certificate path
 * @param ssl_key SSL Private key path
 * @param ssl_ca_cert SSL CA certificate path
 * @param ssl_version SSL version string
 * @param verify_depth SSL certificate verification depth
 * @return 0 on success, non-zero on error
 */
int config_handle_ssl(SERVICE* service,
		      char* mode,
		      char* ssl_cert,
		      char* ssl_key,
		      char* ssl_ca_cert,
		      char* ssl_version,
		      char* verify_depth)
{
    int error_count = 0;
    if(mode && strcmp(mode,"disabled") == 0)
    {
	if(serviceSetSSL(service,mode) != 0)
	{
	    mxs_log(LE,"Error: Unknown parameter for service '%s': %s",service->name,mode);
	    error_count++;
	}
	return error_count;
    }


    if(ssl_cert == NULL)
    {
	error_count++;
	mxs_log(LE,"Error: Server certificate missing for service '%s'."
		"Please provide the path to the server certificate by adding the ssl_cert=<path> parameter",
		 service->name);
    }
    else if(access(ssl_cert,F_OK) != 0)
    {
	mxs_log(LE,"Error: Server certificate file for service '%s' not found: %s",
		 service->name,
		 ssl_cert);
	error_count++;
    }

    if(ssl_ca_cert == NULL)
    {
	error_count++;
	mxs_log(LE,"Error: CA Certificate missing for service '%s'."
		"Please provide the path to the certificate authority certificate by adding the ssl_ca_cert=<path> parameter",
		 service->name);
    }
    else if(access(ssl_ca_cert,F_OK) != 0)
    {
	mxs_log(LE,"Error: Certificate authority file for service '%s' not found: %s",
		 service->name,
		 ssl_ca_cert);
	error_count++;
    }

    if(ssl_key == NULL)
    {
	error_count++;
	mxs_log(LE,"Error: Server private key missing for service '%s'. "
		"Please provide the path to the server certificate key by adding the ssl_key=<path> parameter"
		,service->name);
    }
    else if(access(ssl_key,F_OK) != 0)
    {
	mxs_log(LE,"Error: Server private key file for service '%s' not found: %s",
		 service->name,
		 ssl_key);
	error_count++;
    }
    
    
    

    if(error_count == 0)
    {
	if(serviceSetSSL(service,mode) != 0)
	{
	    mxs_log(LE,"Error: Unknown parameter for service '%s': %s",service->name,mode);
	    error_count++;
	}
	else
	{
	    serviceSetCertificates(service,ssl_cert,ssl_key,ssl_ca_cert);
	    if(ssl_version)
	    {
		if(serviceSetSSLVersion(service,ssl_version) != 0)
		{
		    mxs_log(LE,"Error: Unknown parameter value for 'ssl_version' for service '%s': %s",service->name,ssl_version);
		    error_count++;
		}
	    }
	    if(verify_depth)
	    {
		if(serviceSetSSLVerifyDepth(service,atoi(verify_depth)) != 0)
		{
		    mxs_log(LE,"Error: Invalid parameter value for 'ssl_cert_verify_depth' for service '%s': %s",service->name,verify_depth);
		    error_count++;
		}
	    }
	}
    }
    return error_count;
}

/**
 * Load the configuration file for the MaxScale
 *
 * @param file	The filename of the configuration file
 * @return A zero return indicates a fatal error reading the configuration
 */
int
config_load(char *file)
{
CONFIG_CONTEXT	config;
int		rval;

	MYSQL *conn;
	conn = mysql_init(NULL);
	if (conn) {
		if (mysql_real_connect(conn, NULL, NULL, NULL, NULL, 0, NULL, 0)) {
			char *ptr,*tmp;
			
			tmp = (char *)mysql_get_server_info(conn);
			unsigned int server_version = mysql_get_server_version(conn);
			
			if(version_string)
			    free(version_string);

			if((version_string = malloc(strlen(tmp) + strlen("5.5.5-") + 1)) == NULL)
			    return 0;

			if (server_version >= 100000)
			{
			    strcpy(version_string,"5.5.5-");
			    strcat(version_string,tmp);
			}
			else
			{
			    strcpy(version_string,tmp);
			}

			ptr = strstr(version_string, "-embedded");
			if (ptr) {
				*ptr = '\0';
			}
			

		}
		mysql_close(conn);
	}

	global_defaults();
	feedback_defaults();

	config.object = "";
	config.next = NULL;

	if (ini_parse(file, handler, &config) < 0)
		return 0;

	config_file = file;

	check_config_objects(config.next);
	rval = process_config_context(config.next);
	free_config_context(config.next);
	monitorStartAll();
	return rval;
}
/**
 * Start the configuration reload process. This closes all current connections,
 * prevents new connections from being made by closing the listeners and stops all
 * monitors.
 */
void config_start_reload()
{
    serviceCloseAll();
    dcb_close_all();
    monitorStopAll();
    reload_state = RELOAD_PREPARED;
}

/**
 * Reload the configuration file for the MaxScale.
 * This processes the configuration file, loads or updates all the modules
 * in the configuration file and disables obsolete modules. After all relevant
 * modules have been loaded or updated, the services and monitors are started.
 * @return A zero return indicates a fatal error reading the configuration
 */
void
config_reload_active()
{
    CONFIG_CONTEXT	config;

    if (!config_file)
	return;

	    reload_state = RELOAD_ACTIVE;
	    if (gateway.version_string)
		free(gateway.version_string);

	    global_defaults();

	    config.object = "";
	    config.next = NULL;

	    hashtable_free(monitorhash);

	    if((monitorhash = hashtable_alloc(5,simple_str_hash,strcmp)) == NULL)
	    {
		mxs_log(LOGFILE_ERROR,"Error: Failed to allocate monitor configuration check hashtable.");
		return;
	    }

	    hashtable_memory_fns(monitorhash,(HASHMEMORYFN)strdup,NULL,(HASHMEMORYFN)free,NULL);

	    /** Clear housekeeper tasks */
	    hktask_clear();

	    if (ini_parse(config_file, handler, &config) < 0)
		return;

	    process_config_update(config.next);
	    free_config_context(config.next);

	    /** Enable/disable feedback task */
	    config_disable_feedback_task();
	    if(feedback.feedback_enable)
		config_enable_feedback_task();

	    serviceRestartAll();
	    monitorStartAll();
	    reload_state = RELOAD_INACTIVE;
}

/**
 * Check the state of the configuration reload. If the configuration reload is set
 * as started, prepare MaxScale for the configuration reload. If the configuration reload
 * is prepared, do the actual configuration reload only if all DCBs have been closed
 * and processed.
 */
void config_reload()
{
    if(spinlock_acquire_nowait(&reload_lock))
    {
	switch(reload_state)
	{
	case RELOAD_START:
	    config_start_reload();
	    break;
	case RELOAD_PREPARED:
	    if(dcb_all_closed())
		config_reload_active();
	    break;
	default:
	    /** No reload is needed or reload is already active*/
	    break;
	}
	spinlock_release(&reload_lock);
    }
}

/***/
int
config_read_config(CONFIG_CONTEXT* context)
{
    if (!config_file)
	return -1;

    context->object = "";
    context->next = NULL;

    if (ini_parse(config_file, handler, context) < 0)
	return -1;

    return 0;
}

void
config_free_config(CONFIG_CONTEXT* context)
{
    free_config_context(context);
}
/**
 * Process a configuration context and turn it into the set of object
 * we need.
 *
 * @param context	The configuration data
 * @return A zero result indicates a fatal error
 */
static	int
process_config_context(CONFIG_CONTEXT *context)
{
CONFIG_CONTEXT		*obj;
int			error_count = 0;

if((monitorhash = hashtable_alloc(5,simple_str_hash,strcmp)) == NULL)
{
    mxs_log(LOGFILE_ERROR,"Error: Failed to allocate monitor configuration check hashtable.");
    return 0;
}

    /**
	 * Process the data and create the services and servers defined
	 * in the data.
	 */
	obj = context;
	while (obj)
	{
		char *type = config_get_value(obj->parameters, "type");
		if (type == NULL)
		{
			LOGIF(LE, (mxs_log_flush(
                                LOGFILE_ERROR,
                                "Error : Configuration object '%s' has no type.",
                                obj->object)));
			error_count++;
		}
		else if (!strcmp(type, "service"))
		{
                        char *router = config_get_value(obj->parameters,
                                                        "router");
                        if (router)
                        {
                                char* max_slave_conn_str;
                                char* max_slave_rlag_str;
				char *user;
				char *auth;
				char *enable_root_user;
				char *connection_timeout;
				char *auth_all_servers;
				char *optimize_wildcard;
				char *strip_db_esc;
				char *weightby;
				char *version_string;
				char *subservices;
				char *ssl,*ssl_cert,*ssl_key,*ssl_ca_cert,*ssl_version;
				char* ssl_cert_verify_depth;
				bool  is_rwsplit = false;
				bool  is_schemarouter = false;
				char *allow_localhost_match_wildcard_host;

				obj->element = service_alloc(obj->object, router);
				user = config_get_value(obj->parameters, "user");
				auth = config_get_value(obj->parameters, "passwd");
				subservices = config_get_value(obj->parameters, "subservices");
				ssl = config_get_value(obj->parameters, "ssl");
				ssl_cert = config_get_value(obj->parameters, "ssl_cert");
				ssl_key = config_get_value(obj->parameters, "ssl_key");
				ssl_ca_cert = config_get_value(obj->parameters, "ssl_ca_cert");
				ssl_version = config_get_value(obj->parameters, "ssl_version");
				ssl_cert_verify_depth = config_get_value(obj->parameters, "ssl_cert_verify_depth");
				enable_root_user = config_get_value(
							obj->parameters, 
							"enable_root_user");

				connection_timeout = 
					config_get_value(
						obj->parameters,
						"connection_timeout");

				optimize_wildcard =
					config_get_value(
						obj->parameters, 
						"optimize_wildcard");

				auth_all_servers =
					config_get_value(
						obj->parameters,
						"auth_all_servers");

				strip_db_esc = 
					config_get_value(
						obj->parameters, 
						"strip_db_esc");
                
				allow_localhost_match_wildcard_host =
					config_get_value(obj->parameters, 
							"localhost_match_wildcard_host");

				weightby = config_get_value(obj->parameters, "weightby");
			
				version_string = config_get_value(obj->parameters, 
								  "version_string");

				/** flag for rwsplit-specific parameters */
				if (strncmp(router, "readwritesplit", strlen("readwritesplit")+1) == 0)
				{
					is_rwsplit = true;
				}

				allow_localhost_match_wildcard_host =
                                        config_get_value(obj->parameters, "localhost_match_wildcard_host");

                                if (obj->element == NULL) /*< if module load failed */
                                {
					LOGIF(LE, (mxs_log_flush(
                                                LOGFILE_ERROR,
                                                "Error : Reading configuration "
                                                "for router service '%s' failed. "
                                                "Router %s is not loaded.",
                                                obj->object,
                                                obj->object)));
                                        error_count++;
                                        break; /*< process next obj */
                                }

				if(subservices)
				{
				    service_set_param_value(obj->element,
						     obj->parameters,
						     subservices,
						     1,STRING_TYPE);
				}

                                if (version_string) {

				    /** Add the 5.5.5- string to the start of the version string if
				     * the version string starts with "10.".
				     * This mimics MariaDB 10.0 replication which adds 5.5.5- for backwards compatibility. */
				    if(strncmp(version_string,"10.",3) == 0)
				    {
					if((((SERVICE *)(obj->element))->version_string = malloc((strlen(version_string) +
						strlen("5.5.5-") + 1) * sizeof(char))) == NULL)
					{
					    mxs_log(LE,"[%s] Error: Memory allocation failed when allocating server string.",
						     __FUNCTION__);
					    error_count++;
					    break;
					}
					strcpy(((SERVICE *)(obj->element))->version_string,"5.5.5-");
					strcat(((SERVICE *)(obj->element))->version_string,version_string);
				    }
				    else
				    {
					((SERVICE *)(obj->element))->version_string = strdup(version_string);
				    }
				} else {
					if (gateway.version_string)
						((SERVICE *)(obj->element))->version_string = strdup(gateway.version_string);
				}
                                max_slave_conn_str = 
                                        config_get_value(obj->parameters, 
                                                         "max_slave_connections");
                                        
                                max_slave_rlag_str = 
                                        config_get_value(obj->parameters, 
                                                         "max_slave_replication_lag");

				if(ssl)
				{
				    error_count += config_handle_ssl(obj->element,
								     ssl,
								     ssl_cert,
								     ssl_key,
								     ssl_ca_cert,
								     ssl_version,
								     ssl_cert_verify_depth);
				}

				if (enable_root_user)
					serviceEnableRootUser(
                                                obj->element, 
                                                config_truth_value(enable_root_user));

				if (connection_timeout)
					serviceSetTimeout(
                                      obj->element, 
                                      atoi(connection_timeout));

				if(auth_all_servers)
					serviceAuthAllServers(obj->element, 
						config_truth_value(auth_all_servers));

				if(optimize_wildcard)
					serviceOptimizeWildcard(obj->element,
						config_truth_value(optimize_wildcard));

				if(strip_db_esc)
					serviceStripDbEsc(obj->element, 
						config_truth_value(strip_db_esc));

				if (weightby)
					serviceWeightBy(obj->element, weightby);

				if (allow_localhost_match_wildcard_host)
					serviceEnableLocalhostMatchWildcardHost(
						obj->element,
						config_truth_value(allow_localhost_match_wildcard_host));

				if (!auth)
					auth = config_get_value(obj->parameters, 
                                                                "auth");

				if (obj->element && user && auth)
				{
					serviceSetUser(obj->element, 
                                                       user, 
                                                       auth);
				}
				else if (user && auth == NULL)
				{
					LOGIF(LE, (mxs_log_flush(
		                                LOGFILE_ERROR,
               			                "Error : Service '%s' has a "
						"user defined but no "
						"corresponding password.",
		                                obj->object)));
				}
				/** Read, validate and set max_slave_connections */
				if (max_slave_conn_str != NULL)
                                {
                                        CONFIG_PARAMETER* param;
                                        bool              succp;
                                        
                                        param = config_get_param(obj->parameters, 
                                                                 "max_slave_connections");
                                        
					if (param == NULL)
					{
						succp = false;
					}
					else
					{
						succp = service_set_param_value(
								obj->element,
								param,
								max_slave_conn_str, 
								COUNT_ATMOST,
								(COUNT_TYPE|PERCENT_TYPE));
					}
					
                                        if (!succp)
                                        {
                                                LOGIF((LM|LE), (mxs_log(
                                                        (LM|LE),
                                                        "* Warning : invalid value type "
                                                        "for parameter \'%s.%s = %s\'\n\tExpected "
                                                        "type is either <int> for slave connection "
                                                        "count or\n\t<int>%% for specifying the "
                                                        "maximum percentage of available the "
                                                        "slaves that will be connected.",
                                                        ((SERVICE*)obj->element)->name,
                                                        param->name,
                                                        param->value)));
                                        }
                                }
                                /** Read, validate and set max_slave_replication_lag */
                                if (max_slave_rlag_str != NULL)
                                {
                                        CONFIG_PARAMETER* param;
                                        bool              succp;
                                        
                                        param = config_get_param(
                                                obj->parameters, 
                                                "max_slave_replication_lag");
                                        
					if (param == NULL)
					{
						succp = false;
					}
					else
					{
						succp = service_set_param_value(
							obj->element,
							param,
							max_slave_rlag_str,
							COUNT_ATMOST,
							COUNT_TYPE);
					}
					
                                        if (!succp)
                                        {
                                                LOGIF(LM, (mxs_log(
                                                        LOGFILE_MESSAGE,
                                                        "* Warning : invalid value type "
                                                        "for parameter \'%s.%s = %s\'\n\tExpected "
                                                        "type is <int> for maximum "
                                                        "slave replication lag.",
                                                        ((SERVICE*)obj->element)->name,
                                                        param->name,
                                                        param->value)));
                                        }
                                }
                                /** Parameters for rwsplit router only */
                                if (is_rwsplit)
				{
					CONFIG_PARAMETER* param;
					char*             use_sql_variables_in;
					bool              succp;
					
					use_sql_variables_in = 
						config_get_value(obj->parameters,
								 "use_sql_variables_in");
					
					if (use_sql_variables_in != NULL)
					{
						param = config_get_param(
								obj->parameters,
								"use_sql_variables_in");
						
						if (param == NULL)
						{
							succp = false;
						}
						else
						{
							succp = service_set_param_value(obj->element,
											param,
											use_sql_variables_in,
											COUNT_NONE,
											SQLVAR_TARGET_TYPE);
						}
						
						if (!succp)
						{
							if(param){
							LOGIF(LM, (mxs_log(
								LOGFILE_MESSAGE,
								"* Warning : invalid value type "
								"for parameter \'%s.%s = %s\'\n\tExpected "
								"type is [master|all] for "
								"use sql variables in.",
								((SERVICE*)obj->element)->name,
								param->name,
								param->value)));
							}else{
								LOGIF(LE, (mxs_log(
								LOGFILE_ERROR,
								"Error : parameter was NULL")));
							
							}
						}
					}
				} /*< if (rw_split) */
			} /*< if (router) */
			else
			{
				obj->element = NULL;
				LOGIF(LE, (mxs_log_flush(
                                        LOGFILE_ERROR,
                                        "Error : No router defined for service "
                                        "'%s'\n",
                                        obj->object)));
				error_count++;
			}
		}
		else if (!strcmp(type, "server"))
		{
                        char *address;
			char *port;
			char *protocol;
			char *monuser;
			char *monpw;

                        address = config_get_value(obj->parameters, "address");
			port = config_get_value(obj->parameters, "port");
			protocol = config_get_value(obj->parameters, "protocol");
			monuser = config_get_value(obj->parameters,
                                                   "monitoruser");
			monpw = config_get_value(obj->parameters, "monitorpw");

			if (address && port && protocol)
			{
				obj->element = server_alloc(address,
                                                            protocol,
                                                            atoi(port));
				if(obj->element == NULL)
				{
				    mxs_log(LE,"Error: Server creation failed: %s:%d(%s)",
					     address,port,protocol);
				    error_count++;
				    break;
				}
				server_set_unique_name(obj->element, obj->object);
			}
			else
			{
				obj->element = NULL;
				LOGIF(LE, (mxs_log_flush(
                                        LOGFILE_ERROR,
                                        "Error : Server '%s' is missing a "
                                        "required configuration parameter. A "
                                        "server must "
                                        "have address, port and protocol "
                                        "defined.",
                                        obj->object)));
				error_count++;
			}
			if (obj->element && monuser && monpw)
				serverAddMonUser(obj->element, monuser, monpw);
			else if (monuser && monpw == NULL)
			{
				LOGIF(LE, (mxs_log_flush(
	                                LOGFILE_ERROR,
					"Error : Server '%s' has a monitoruser"
					"defined but no corresponding password.",
                                        obj->object)));
			}
			if (obj->element)
			{
                                SERVER *server = obj->element;
                                server->persistpoolmax = strtol(config_get_value_string(obj->parameters, "persistpoolmax"), NULL, 0);
                                server->persistmaxtime = strtol(config_get_value_string(obj->parameters, "persistmaxtime"), NULL, 0);
				CONFIG_PARAMETER *params = obj->parameters;
				while (params)
				{
					if (strcmp(params->name, "address")
						&& strcmp(params->name, "port")
						&& strcmp(params->name,
								"protocol")
						&& strcmp(params->name,
								"monitoruser")
						&& strcmp(params->name,
								"monitorpw")
						&& strcmp(params->name,
								"type")
						&& strcmp(params->name,
								"persistpoolmax")
						&& strcmp(params->name,
								"persistmaxtime")
						)
					{
						serverAddParameter(obj->element,
							params->name,
							params->value);
					}
					params = params->next;
				}
			}
		}
		else if (!strcmp(type, "filter"))
		{
		    if(config_add_filter(obj) != 0)
		    {
			mxs_log(LE,"Error: Failed to configure filter '%s'.",obj->object);
		    }
		}
		obj = obj->next;
	}

	/*
	 * Now we have the services we can add the servers to the services
	 * add the protocols to the services
	 */
	obj = context;
	while (obj)
	{
		char *type = config_get_value(obj->parameters, "type");
		if (type == NULL)
			;
		else if (!strcmp(type, "service"))
		{
                        char *servers;
			char *roptions;
			char *router;
                        char *filters = config_get_value(obj->parameters,
                                                        "filters");
			servers = config_get_value(obj->parameters, "servers");
			roptions = config_get_value(obj->parameters,
                                                    "router_options");
			router = config_get_value(obj->parameters, "router");
			if (servers && obj->element)
			{
				char *lasts;
				char *s = strtok_r(servers, ",", &lasts);
				while (s)
				{
					CONFIG_CONTEXT *obj1 = context;
					int	found = 0;
					while (obj1)
					{
						if (strcmp(trim(s), obj1->object) == 0 &&
                                                    obj->element && obj1->element)
                                                {
							found = 1;
							serviceAddBackend(
                                                                obj->element,
                                                                obj1->element);
                                                }
						obj1 = obj1->next;
					}
					if (!found)
					{
						LOGIF(LE, (mxs_log_flush(
		                                        LOGFILE_ERROR,
							"Error: Unable to find "
							"server '%s' that is "
							"configured as part of "
							"service '%s'.",
							s, obj->object)));
					}
					s = strtok_r(NULL, ",", &lasts);
				}
			}
			else if (servers == NULL && internalService(router) == 0)
			{
				LOGIF(LE, (mxs_log_flush(
                                        LOGFILE_ERROR,
                                        "Warning: The service '%s' is missing a "
                                        "definition of the servers that provide "
                                        "the service.",
                                        obj->object)));
			}
			if (roptions && obj->element)
			{
				char *lasts;
				char *s = strtok_r(roptions, ",", &lasts);
				while (s)
				{
					serviceAddRouterOption(obj->element, s);
					s = strtok_r(NULL, ",", &lasts);
				}
			}
			if (filters && obj->element)
			{
				if(serviceSetFilters(obj->element, filters) != 0)
				{
				    error_count++;
				}
			}
		}
		else if (!strcmp(type, "listener"))
		{
                        char *service;
			char *address;
			char *port;
			char *protocol;
			char *socket;
			struct sockaddr_in serv_addr;

                        service = config_get_value(obj->parameters, "service");
			port = config_get_value(obj->parameters, "port");
			address = config_get_value(obj->parameters, "address");
			protocol = config_get_value(obj->parameters, "protocol");
			socket = config_get_value(obj->parameters, "socket");

			/* if id is not set, do it now */
			if (gateway.id == 0) {
				setipaddress(&serv_addr.sin_addr, (address == NULL) ? "0.0.0.0" : address);
				gateway.id = (unsigned long) (serv_addr.sin_addr.s_addr + (port != NULL ? atoi(port) : 0 + getpid()));
			}

			if(service && protocol && (socket || port))
			{
			    if (socket)
			    {
				CONFIG_CONTEXT *ptr = context;
				while (ptr && strcmp(ptr->object, service) != 0)
				    ptr = ptr->next;
				if (ptr && ptr->element)
				{
				    serviceAddProtocol(ptr->element,
						     protocol,
						     socket,
						     0);
				} 
				else
				{
				    LOGIF(LE, (mxs_log_flush(
					    LOGFILE_ERROR,
					    "Error : Listener '%s', "
					    "service '%s' not found. "
					    "Listener will not execute for socket %s.",
					    obj->object, service, socket)));
				    error_count++;
				}
			    }

			    if (port)
			    {
				CONFIG_CONTEXT *ptr = context;
				while (ptr && strcmp(ptr->object, service) != 0)
				    ptr = ptr->next;
				if (ptr && ptr->element)
				{
				    serviceAddProtocol(ptr->element,
						     protocol,
						     address,
						     atoi(port));
				}
				else
				{
				    LOGIF(LE, (mxs_log_flush(
					    LOGFILE_ERROR,
					    "Error : Listener '%s', "
					    "service '%s' not found. "
					    "Listener will not execute.",
					    obj->object, service)));
				    error_count++;
				}
			    }
			}
			else
			{
			    LOGIF(LE, (mxs_log_flush(
				    LOGFILE_ERROR,
				    "Error : Listener '%s' is missing a "
				    "required "
				    "parameter. A Listener must have a "
				    "service, port and protocol defined.",
				    obj->object)));
			    error_count++;
			}
		}
		else if (!strcmp(type, "monitor"))
		{
			error_count += config_add_monitor(obj, context, NULL);
		}
		else if (strcmp(type, "server") != 0
			&& strcmp(type, "filter") != 0)
		{
			LOGIF(LE, (mxs_log_flush(
                                LOGFILE_ERROR,
                                "Error : Configuration object '%s' has an "
                                "invalid type specified.",
                                obj->object)));
			error_count++;
		}

		obj = obj->next;
	} /*< while */
	
	if (error_count)
	{
		LOGIF(LE, (mxs_log_flush(
                        LOGFILE_ERROR,
                        "Error : %d errors where encountered processing the "
                        "configuration file '%s'.",
                        error_count,
                        config_file)));
		return 0;
	}
	return 1;
}

/**
 * Get the value of a config parameter
 *
 * @param params	The linked list of config parameters
 * @param name		The parameter to return
 * @return the parameter value or NULL if not found
 */
static char *
config_get_value(CONFIG_PARAMETER *params, const char *name)
{
	while (params)
	{
		if (!strcmp(params->name, name))
			return params->value;
		params = params->next;
	}
	return NULL;
}

/**
 * Get the value of a config parameter as a string
 *
 * @param params	The linked list of config parameters
 * @param name		The parameter to return
 * @return the parameter value or null string if not found
 */
static const char *
config_get_value_string(CONFIG_PARAMETER *params, const char *name)
{
	while (params)
	{
		if (!strcmp(params->name, name))
			return (const char *)params->value;
		params = params->next;
	}
	return "";
}


CONFIG_PARAMETER* config_get_param(
        CONFIG_PARAMETER* params, 
        const char*       name)
{
        while (params)
        {
                if (!strcmp(params->name, name))
                        return params;
                params = params->next;
        }
        return NULL;
}

config_param_type_t config_get_paramtype(
        CONFIG_PARAMETER* param)
{
        return param->qfd_param_type;
}

bool config_get_valint(
	int*                val,
        CONFIG_PARAMETER*   param,
        const char*         name, /*< if NULL examine current param only */
        config_param_type_t ptype)
{       
	bool succp = false;;
	
	ss_dassert((ptype == COUNT_TYPE || ptype == PERCENT_TYPE) && param != NULL);
        
        while (param)
        {
                if (name == NULL || !strncmp(param->name, name, MAX_PARAM_LEN))
                {
                        switch (ptype) {
                                case COUNT_TYPE:
                                        *val = param->qfd.valcount;
					succp = true;
                                        goto return_succp;
                                        
                                case PERCENT_TYPE:
                                        *val = param->qfd.valpercent;
					succp  =true;
                                        goto return_succp;

				default:
                                        goto return_succp;
                        }
                } 
                param = param->next;
        }
return_succp:
        return succp;
}


bool config_get_valbool(
	bool*               val,
	CONFIG_PARAMETER*   param,
	const char*         name,
	config_param_type_t ptype)
{
	bool succp;
	
	ss_dassert(ptype == BOOL_TYPE);
	ss_dassert(param != NULL);
	
	if (ptype != BOOL_TYPE || param == NULL)
	{
		succp = false;
		goto return_succp;
	}
	
	while (param)
	{
		if (name == NULL || !strncmp(param->name, name, MAX_PARAM_LEN))
		{
			*val = param->qfd.valbool;
			succp = true;
			goto return_succp;
		} 
		param = param->next;
	}
	succp = false;
	
return_succp:
	return succp;
		
}


bool config_get_valtarget(
	target_t*           val,
	CONFIG_PARAMETER*   param,
	const char*         name,
	config_param_type_t ptype)
{
	bool succp;
	
	ss_dassert(ptype == SQLVAR_TARGET_TYPE);
	ss_dassert(param != NULL);
	
	if (ptype != SQLVAR_TARGET_TYPE || param == NULL)
	{
		succp = false;
		goto return_succp;
	}
	
	while (param)
	{
		if (name == NULL || !strncmp(param->name, name, MAX_PARAM_LEN))
		{
			*val = param->qfd.valtarget;
			succp = true;
			goto return_succp;
		} 
		param = param->next;
	}
	succp = false;
	
return_succp:
	return succp;
	
}

CONFIG_PARAMETER* config_clone_param(
        CONFIG_PARAMETER* param)
{
        CONFIG_PARAMETER* p2;
        
        p2 = (CONFIG_PARAMETER*) malloc(sizeof(CONFIG_PARAMETER));
        
        if (p2 == NULL)
        {
                goto return_p2;
        }
        p2->name = strndup(param->name, MAX_PARAM_LEN);
        p2->value = strndup(param->value, MAX_PARAM_LEN);
	p2->qfd_param_type = param->qfd_param_type;
        p2->next = param->next;

        switch (param->qfd_param_type)
        {
	case STRING_TYPE:
		if(param->qfd.valstr == NULL)
		{
		    param->qfd.valstr = strdup(param->value);
		}
                p2->qfd.valstr = strndup(param->qfd.valstr, MAX_PARAM_LEN);
		break;
	case PERCENT_TYPE:
	    p2->qfd.valpercent = param->qfd.valpercent;
	    break;
	case COUNT_TYPE:
	    p2->qfd.valcount= param->qfd.valcount;
	    break;
	case SQLVAR_TARGET_TYPE:
	    p2->qfd.valtarget= param->qfd.valtarget;
	    break;
	case BOOL_TYPE:
	    p2->qfd.valbool= param->qfd.valbool;
	    break;
	default:
	    break;
        }
                        
return_p2:
        return p2;
}

/**
 * Free a config tree
 *
 * @param context	The configuration data
 */
static	void
free_config_context(CONFIG_CONTEXT *context)
{
CONFIG_CONTEXT		*obj;
CONFIG_PARAMETER	*p1, *p2;

	while (context)
	{
		free(context->object);
		p1 = context->parameters;
		while (p1)
		{
			free(p1->name);
			free(p1->value);
			p2 = p1->next;
			free(p1);
			p1 = p2;
		}
		obj = context->next;
		free(context);
		context = obj;
	}
}

/**
 * Return the number of configured threads
 *
 * @return The number of threads configured in the config file
 */
int
config_threadcount()
{
	return gateway.n_threads;
}

/**
 * Return the number of non-blocking polls to be done before a blocking poll
 * is issued.
 *
 * @return The number of blocking poll calls to make before a blocking call
 */
unsigned int
config_nbpolls()
{
	return gateway.n_nbpoll;
}

/**
 * Return the configured number of milliseconds for which we wait when we do
 * a blocking poll call.
 *
 * @return The number of milliseconds to sleep in a blocking poll call
 */
unsigned int
config_pollsleep()
{
	return gateway.pollsleep;
}

/**
 * Return the feedback config data pointer
 *
 * @return  The feedback config data pointer
 */
FEEDBACK_CONF *
config_get_feedback_data()
{
	return &feedback;
}

static struct {
	char		*logname;
	logfile_id_t	logfile;
} lognames[] = {
	{ "log_messages", LOGFILE_MESSAGE },
	{ "log_trace", LOGFILE_TRACE },
	{ "log_debug", LOGFILE_DEBUG },
	{ NULL, 0 }
};
/**
 * Configuration handler for items in the global [MaxScale] section
 *
 * @param name	The item name
 * @param value	The item value
 * @return 0 on error
 */
static	int
handle_global_item(const char *name, const char *value)
{
int i;
	if (strcmp(name, "threads") == 0)
	{
		gateway.n_threads = atoi(value);
	}
	else if (strcmp(name, "non_blocking_polls") == 0)
	{ 
		gateway.n_nbpoll = atoi(value);
	}
	else if (strcmp(name, "poll_sleep") == 0)
	{
		gateway.pollsleep = atoi(value);
        }
	else if (strcmp(name, "ms_timestamp") == 0)
	{
		mxs_set_highp(config_truth_value((char*)value));
	}
	else
	{
		for (i = 0; lognames[i].logname; i++)
		{
			if (strcasecmp(name, lognames[i].logname) == 0)
			{
				if (config_truth_value((char*)value))
					mxs_log_enable(lognames[i].logfile);
				else
					mxs_log_disable(lognames[i].logfile);
			}
		}
        }
	return 1;
}

/**
 * Configuration handler for items in the feedback [feedback] section
 *
 * @param name	The item name
 * @param value	The item value
 * @return 0 on error
 */
static	int
handle_feedback_item(const char *name, const char *value)
{
int i;
	if (strcmp(name, "feedback_enable") == 0)
	{
		feedback.feedback_enable = config_truth_value((char *)value);
	}
	else if (strcmp(name, "feedback_user_info") == 0)
	{
		if(feedback.feedback_user_info)
		    free(feedback.feedback_user_info);
		feedback.feedback_user_info = strdup(value);
        }
	else if (strcmp(name, "feedback_url") == 0)
	{
		if(feedback.feedback_url)
		    free(feedback.feedback_url);
		feedback.feedback_url = strdup(value);
        }
	if (strcmp(name, "feedback_timeout") == 0)
	{
		feedback.feedback_timeout = atoi(value);
	}
	if (strcmp(name, "feedback_connect_timeout") == 0)
	{
		feedback.feedback_connect_timeout = atoi(value);
	}
	if (strcmp(name, "feedback_frequency") == 0)
	{
		feedback.feedback_frequency = atoi(value);
	}
	return 1;
}

/**
 * Set the defaults for the global configuration options
 */
static void
global_defaults()
{
	uint8_t mac_addr[6]="";
	struct utsname uname_data;
	gateway.n_threads = 1;
	gateway.n_nbpoll = DEFAULT_NBPOLLS;
	gateway.pollsleep = DEFAULT_POLLSLEEP;
	if (version_string != NULL)
		gateway.version_string = strdup(version_string);
	else
		gateway.version_string = NULL;
	gateway.id=0;

	/* get release string */
	if(!config_get_release_string(gateway.release_string))
	    sprintf(gateway.release_string,"undefined");

	/* get first mac_address in SHA1 */
	if(config_get_ifaddr(mac_addr)) {
		gw_sha1_str(mac_addr, 6, gateway.mac_sha1);
	} else {
		memset(gateway.mac_sha1, '\0', sizeof(gateway.mac_sha1));
		memcpy(gateway.mac_sha1, "MAC-undef", 9);
	}

	/* get uname info */
	if (uname(&uname_data))
		strcpy(gateway.sysname, "undefined");
	else
		strncpy(gateway.sysname, uname_data.sysname, _SYSNAME_STR_LENGTH);
}

/**
 * Set the defaults for the feedback configuration options
 */
static void
feedback_defaults()
{
	feedback.feedback_enable = 0;
	feedback.feedback_user_info = NULL;
	feedback.feedback_last_action = _NOTIFICATION_SEND_PENDING;
	feedback.feedback_timeout = _NOTIFICATION_OPERATION_TIMEOUT;
	feedback.feedback_connect_timeout = _NOTIFICATION_CONNECT_TIMEOUT;
	feedback.feedback_url = NULL;
	feedback.feedback_frequency = 1800;
	feedback.release_info = gateway.release_string;
	feedback.sysname = gateway.sysname;
	feedback.mac_sha1 = gateway.mac_sha1;
}

/**
 * Process a configuration context update and turn it into the set of object
 * we need.
 *
 * @param context	The configuration data
 */
static	int
process_config_update(CONFIG_CONTEXT *context)
{
CONFIG_CONTEXT		*obj;

        /** Remove old servers before adding any new or renamed ones */
	server_free_all();

	/** Disable obsolete services */
	serviceRemoveObsolete(context);
	serviceRemoveAllProtocols();

	/** Disable obsolete monitors */
	monitor_disable_obsolete(context);

	/**
	 * Process the data and create the services and servers defined
	 * in the data.
	 */
	obj = context;
	while (obj)
	{
		char *type = config_get_value(obj->parameters, "type");
		if (type == NULL)
                {
                    LOGIF(LE,
                          (mxs_log_flush(
                                  LOGFILE_ERROR,
                                  "Error : Configuration object %s has no type.",
                                  obj->object)));
                }
		else if (!strcmp(type, "service"))
		{
			config_service_update(obj);

		}
		else if (!strcmp(type, "server"))
		{
			config_server_update(obj);
		}
		else if (!strcmp(type, "filter"))
		{
		    if(filter_find(obj->object) == NULL &&
		     config_add_filter(obj) != 0)
		    {
			mxs_log(LE,"Error: Failed to configure filter '%s'.",obj->object);
		    }
		}

		obj = obj->next;
	}

	/*
	 * Now we have the services we can add the servers to the services
	 * add the protocols to the services
	 */
	obj = context;
	while (obj)
	{
		char *type = config_get_value(obj->parameters, "type");
		if (type == NULL)
			;
		else if (!strcmp(type, "service"))
		{
			config_service_update_objects(obj, context);
#ifdef SS_DEBUG
			service_debug_show(obj->object);
#endif
		}
		else if (!strcmp(type, "monitor"))
		{
			config_monitor_update(obj, context);
		}
		else if (type && strcmp(type, "listener") == 0)
		{
		    config_listener_update(obj, context);
		}
		else if (strcmp(type, "server") != 0 &&
                         strcmp(type, "monitor") != 0 &&
			 strcmp(type, "filter") != 0)
		{
			LOGIF(LE, (mxs_log_flush(
                                LOGFILE_ERROR,
                                "Error : Configuration object %s has an invalid "
                                "type specified.",
                                obj->object)));
		}
		obj = obj->next;
	}
	return 1;
}

static char *service_params[] =
	{
                "type",
                "router",
                "router_options",
                "servers",
                "user",
                "passwd",
                "enable_root_user",
                "connection_timeout",
                "auth_all_servers",
		"optimize_wildcard",
                "strip_db_esc",
                "localhost_match_wildcard_host",
                "max_slave_connections",
                "max_slave_replication_lag",
                "use_sql_variables_in",		/*< rwsplit only */
                "subservices",
                "version_string",
                "filters",
                "weightby",
		"ssl_cert",
		"ssl_ca_cert",
		"ssl",
		"ssl_key",
		"ssl_version",
		"ssl_cert_verify_depth",
                NULL
        };

static char *listener_params[] =
	{
                "type",
                "service",
                "protocol",
                "port",
                "address",
                "socket",
                NULL
        };

static char *monitor_params[] =
	{
                "type",
                "module",
                "servers",
                "user",
                "passwd",
		"script",
		"events",
		"mysql51_replication",
		"monitor_interval",
		"detect_replication_lag",
		"detect_stale_master",
		"disable_master_failback",
		"backend_connect_timeout",
		"backend_read_timeout",
		"backend_write_timeout",
		"available_when_donor",
                "disable_master_role_setting",
                NULL
        };
/**
 * Check the configuration objects have valid parameters
 */
static void
check_config_objects(CONFIG_CONTEXT *context)
{
CONFIG_CONTEXT		*obj;
CONFIG_PARAMETER 	*params;
char			*type, **param_set;
int			i;

	/**
	 * Process the data and create the services and servers defined
	 * in the data.
	 */
	obj = context;
	while (obj)
	{
		param_set = NULL;
		if (obj->parameters &&
			(type = config_get_value(obj->parameters, "type")))
		{
			if (!strcmp(type, "service"))
				param_set = service_params;
			else if (!strcmp(type, "listener"))
				param_set = listener_params;
			else if (!strcmp(type, "monitor"))
				param_set = monitor_params;
		}
		if (param_set != NULL)
		{
			params = obj->parameters;
			while (params)
			{
				int found = 0;
				for (i = 0; param_set[i]; i++)
					if (!strcmp(params->name, param_set[i]))
						found = 1;
				if (found == 0)
					LOGIF(LE, (mxs_log_flush(
                                                LOGFILE_ERROR,
                                                "Error : Unexpected parameter "
                                                "'%s' for object '%s' of type "
                                                "'%s'.",
						params->name,
                                                obj->object,
                                                type)));
				params = params->next;
			}
		}
		obj = obj->next;
	}
}

/**
 * Set qualified parameter value to CONFIG_PARAMETER struct.
 */
bool config_set_qualified_param(
        CONFIG_PARAMETER* param, 
        void* val, 
        config_param_type_t type)
{
        bool succp;
        
        switch (type) {
                case STRING_TYPE:
                        param->qfd.valstr = strndup((const char *)val, MAX_PARAM_LEN);
                        succp = true;
                        break;

                case COUNT_TYPE:
                        param->qfd.valcount = *(int *)val;
                        succp = true;
                        break;
                        
                case PERCENT_TYPE:
                        param->qfd.valpercent = *(int *)val;
                        succp = true;
                        break;
                        
                case BOOL_TYPE:
                        param->qfd.valbool = *(bool *)val;
                        succp = true;
                        break;

		case SQLVAR_TARGET_TYPE:
			param->qfd.valtarget = *(target_t *)val;
			succp = true;
			break;
                default:
                        succp = false;
                        break;
        }
        
        if (succp)
        {
                param->qfd_param_type = type;
        }
        return succp;
}

/**
 * Used for boolean settings where values may be 1, yes or true
 * to enable a setting or -, no, false to disable a setting.
 *
 * @param	str	String to convert to a boolean
 * @return	Truth value
 */
int
config_truth_value(char *str)
{
	if (strcasecmp(str, "true") == 0 || strcasecmp(str, "on") == 0 ||
	 strcasecmp(str, "yes") == 0 || strcasecmp(str, "1") == 0)
	{
		return 1;
	}
	if (strcasecmp(str, "false") == 0 || strcasecmp(str, "off") == 0 ||
	 strcasecmp(str, "no") == 0|| strcasecmp(str, "0") == 0)
	{
		return 0;
	}
	mxs_log(LOGFILE_ERROR,"Error: Not a boolean value: %s",str);
	return -1;
}


/**
 * Converts a string into a floating point representation of a percentage value.
 * For example 75% is converted to 0.75 and -10% is converted to -0.1.
 * @param	str	String to convert
 * @return	String converted to a floating point percentage
 */
double
config_percentage_value(char *str)
{
    double value = 0;

    value = strtod(str,NULL);
    if(value != 0)
	value /= 100.0;

    return value;
}

static char *InternalRouters[] = {
	"debugcli",
	"cli",
	"maxinfo",
	NULL
};

/**
 * Determine if the router is one of the special internal services that
 * MaxScale offers.
 *
 * @param router	The router name
 * @return	Non-zero if the router is in the InternalRouters table
 */
static int
internalService(char *router)
{
int	i;

	if (router)
	{
		for (i = 0; InternalRouters[i]; i++)
			if (strcmp(router, InternalRouters[i]) == 0)
				return 1;
	}
	return 0;
}
/**
 * Get the MAC address of first network interface
 *
 * and fill the provided allocated buffer with SHA1 encoding
 * @param output	Allocated 6 bytes buffer
 * @return 1 on success, 0 on failure
 *
 */
int
config_get_ifaddr(unsigned char *output)
{
	struct ifreq ifr;
	struct ifconf ifc;
	char buf[1024];
	struct ifreq* it;
	struct ifreq* end;
	int success = 0;

	int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (sock == -1) {
		return 0;
	};

	ifc.ifc_len = sizeof(buf);
	ifc.ifc_buf = buf;
	if (ioctl(sock, SIOCGIFCONF, &ifc) == -1) {
		close(sock);
		return 0;
	}

	it = ifc.ifc_req;
	end = it + (ifc.ifc_len / sizeof(struct ifreq));

	for (; it != end; ++it) {
		strcpy(ifr.ifr_name, it->ifr_name);
		if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
			if (! (ifr.ifr_flags & IFF_LOOPBACK)) { /* don't count loopback */
				if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
					success = 1;
					break;
				}
			}
		} else {
		    close(sock);
			return 0;
		}
	}

	if (success)
		memcpy(output, ifr.ifr_hwaddr.sa_data, 6);

	return success;
}

/**
 * Get the linux distribution info
 *
 * @param release	The allocated buffer where
 *			the found distribution is copied into.
 * @return 1 on success, 0 on failure
 *
 */
int
config_get_release_string(char* release)
{
	const char *masks[]= {
		"/etc/*-version", "/etc/*-release",
		"/etc/*_version", "/etc/*_release"
	};

	bool have_distribution;
	char distribution[_RELEASE_STR_LENGTH]="";
	int fd;
	int i;
	char *to;

	have_distribution= false;

	/* get data from lsb-release first */
	if ((fd= open("/etc/lsb-release", O_RDONLY)) != -1)
	{
		/* LSB-compliant distribution! */
		size_t len= read(fd, (char*)distribution, sizeof(distribution)-1);
		close(fd);
		if (len != (size_t)-1)
		{
			distribution[len]= 0;
			char *found= strstr(distribution, "DISTRIB_DESCRIPTION=");
			if (found)
			{
				have_distribution = true;
				char *end = strstr(found, "\n");
				if (end == NULL)
				end = distribution + len;
				found += 20;

				if (*found == '"' && end[-1] == '"')
				{
					found++;
					end--;
				}
				*end = 0;

				to = strcpy(distribution, "lsb: ");
				memmove(to, found, end - found + 1 < INT_MAX ? end - found + 1 : INT_MAX);

				strncpy(release, to, _RELEASE_STR_LENGTH);

				return 1;
			}
		}
	}

	/* if not an LSB-compliant distribution */
	for (i= 0; !have_distribution && i < 4; i++)
	{
		glob_t found;
		char *new_to;

		if (glob(masks[i], GLOB_NOSORT, NULL, &found) == 0)
		{
			int fd;
			int k = 0;
			int skipindex = 0;
			int startindex = 0;

			for (k = 0; k< found.gl_pathc; k++) {
				if (strcmp(found.gl_pathv[k], "/etc/lsb-release") == 0) {
					skipindex = k;
				}
			}

			if ( skipindex == 0)
				startindex++;

			if ((fd= open(found.gl_pathv[startindex], O_RDONLY)) != -1)
			{
			/*
				+5 and -8 below cut the file name part out of the
				full pathname that corresponds to the mask as above.
			*/
				new_to = strncpy(distribution, found.gl_pathv[0] + 5,_RELEASE_STR_LENGTH - 1);
				new_to += 8;
				*new_to++ = ':';
				*new_to++ = ' ';

				size_t to_len= distribution + sizeof(distribution) - 1 - new_to;
				size_t len= read(fd, (char*)new_to, to_len);

				close(fd);

				if (len != (size_t)-1)
				{
					new_to[len]= 0;
					char *end= strstr(new_to, "\n");
					if (end)
						*end= 0;

					have_distribution= true;
					strncpy(release, new_to, _RELEASE_STR_LENGTH);
				}
			}
		}
		globfree(&found);
	}

	if (have_distribution)
		return 1;
	else
		return 0;
}

/**
 * Add the 'send_feedback' task to the task list
 */
void
config_enable_feedback_task(void) {
	FEEDBACK_CONF *cfg = config_get_feedback_data();
	int url_set = 0;
	int user_info_set = 0;
	int enable_set = cfg->feedback_enable;

	url_set = cfg->feedback_url != NULL && strlen(cfg->feedback_url);	
	user_info_set = cfg->feedback_user_info != NULL && strlen(cfg->feedback_user_info);

	if (enable_set && url_set && user_info_set) {
		/* Add the task to the tasl list */
        	if (hktask_add("send_feedback", module_feedback_send, cfg, cfg->feedback_frequency)) {

			LOGIF(LM, (mxs_log_flush(
				LOGFILE_MESSAGE,
				"Notification service feedback task started: URL=%s, User-Info=%s, Frequency %u seconds",
				cfg->feedback_url,
				cfg->feedback_user_info,
				cfg->feedback_frequency)));
		}
	} else {
		if (enable_set) {
			LOGIF(LE, (mxs_log_flush(
				LOGFILE_ERROR,
				"Error: Notification service feedback cannot start: feedback_enable=1 but"
				" some required parameters are not set: %s%s%s",
				url_set == 0 ? "feedback_url is not set" : "", (user_info_set == 0 && url_set == 0) ? ", " : "", user_info_set == 0 ? "feedback_user_info is not set" : "")));
		} else {
			LOGIF(LT, (mxs_log_flush(
				LOGFILE_TRACE,
				"Notification service feedback is not enabled")));
		}
	}
}

/**
 * Remove the 'send_feedback' task
 */
void
config_disable_feedback_task(void) {
        hktask_remove("send_feedback");
}

unsigned long  config_get_gateway_id()
{
    return gateway.id;
}
void config_add_param(CONFIG_CONTEXT* obj, char* key,char* value)
{
    CONFIG_PARAMETER* nptr = malloc(sizeof(CONFIG_PARAMETER));

    if(nptr == NULL)
    {
	mxs_log(LOGFILE_ERROR,"Memory allocation failed when adding configuration parameters");
	return;
    }

    nptr->name = strdup(key);
    nptr->value = strdup(value);
    nptr->next = obj->parameters;
    obj->parameters = nptr;
}

/*
 * Service configuration update
 * Update all services with the new values in the configuration context object.
 * It updates the router parameters and options and the service parameters.
 * @param obj		The config (type=service) object to update
 *
 */

void config_service_update(CONFIG_CONTEXT *obj) {
	SERVICE			*service;
	SERVER			*server;
	char *router = config_get_value(obj->parameters, "router");

	if (router)
	{
		if ((service = service_find(obj->object)) != NULL)
		{
			char *user;
			char *auth;
			char *enable_root_user;

			char *connection_timeout;

			char* auth_all_servers;
			char* optimize_wildcard;
			char* strip_db_esc;
			char* max_slave_conn_str;
			char* max_slave_rlag_str;
			char *version_string;
			char *allow_localhost_match_wildcard_host;
			char *ssl,*ssl_cert,*ssl_key,*ssl_ca_cert,*ssl_version;
			char* ssl_cert_verify_depth;
			char* weightby;

			service_free_parameters(service);

			ssl = config_get_value(obj->parameters, "ssl");
			ssl_cert = config_get_value(obj->parameters, "ssl_cert");
			ssl_key = config_get_value(obj->parameters, "ssl_key");
			ssl_ca_cert = config_get_value(obj->parameters, "ssl_ca_cert");
			ssl_version = config_get_value(obj->parameters, "ssl_version");
			ssl_cert_verify_depth = config_get_value(obj->parameters, "ssl_cert_verify_depth");
			weightby = config_get_value(obj->parameters, "weightby");
			enable_root_user = config_get_value(obj->parameters, "enable_root_user");

			connection_timeout = config_get_value(obj->parameters, "connection_timeout");
			user = config_get_value(obj->parameters, "user");
			auth = config_get_value(obj->parameters, "passwd");
                    
			auth_all_servers = config_get_value(obj->parameters, "auth_all_servers");
			optimize_wildcard = config_get_value(obj->parameters, "optimize_wildcard");
			strip_db_esc = config_get_value(obj->parameters, "strip_db_esc");
			version_string = config_get_value(obj->parameters, "version_string");
			allow_localhost_match_wildcard_host = config_get_value(obj->parameters, "localhost_match_wildcard_host");

			if(service->version_string)
			    free(service->version_string);

			if (version_string) {

			    /** Add the 5.5.5- string to the start of the version string if
			     * the version string starts with "10.".
			     * This mimics MariaDB 10.0 replication which adds 5.5.5- for backwards compatibility. */
			    if(strncmp(version_string,"10.",3) == 0)
			    {
				if((service->version_string = malloc((strlen(version_string) +
											 strlen("5.5.5-") + 1) * sizeof(char))) == NULL)
				{
				    mxs_log(LE,"[%s] Error: Memory allocation failed.",
					     __FUNCTION__);
				}
				strcpy(service->version_string,"5.5.5-");
				strcat(service->version_string,version_string);
			    }
			    else
			    {
				service->version_string = strdup(version_string);
			    }
			} else {
			    if (gateway.version_string)
				service->version_string = strdup(gateway.version_string);
			}

			if (user && auth) {
				service_update(service, router, user, auth);

				serviceEnableRootUser(service, enable_root_user ?
				    config_truth_value(enable_root_user) : 0);

				serviceSetTimeout(service, connection_timeout ?
				    atoi(connection_timeout) : 0);

				serviceAuthAllServers(service, auth_all_servers ?
				    config_truth_value(auth_all_servers) : 0);

				serviceOptimizeWildcard(service, optimize_wildcard ?
				    config_truth_value(optimize_wildcard) : 0);

				serviceStripDbEsc(service, strip_db_esc ?
				    config_truth_value(strip_db_esc) : 0);

				serviceEnableLocalhostMatchWildcardHost(service,
						allow_localhost_match_wildcard_host ?
						    config_truth_value(allow_localhost_match_wildcard_host) : 0);

				serviceWeightBy(service,weightby);
				/** Read, validate and set max_slave_connections */        
				max_slave_conn_str = config_get_value(
							obj->parameters, 
							"max_slave_connections");

				if (max_slave_conn_str != NULL)
				{
					CONFIG_PARAMETER* param;
					bool              succp;
                           
					param = config_get_param(obj->parameters, 
						"max_slave_connections");

					if (param == NULL)
					{
						succp = false;
					}
					else 
					{
						succp = service_set_param_value(
								service,
								param,
								max_slave_conn_str, 
								COUNT_ATMOST,
								(PERCENT_TYPE|COUNT_TYPE));
					}

					if (!succp && param != NULL)
					{
						LOGIF(LM, (mxs_log(
							LOGFILE_MESSAGE,
							"* Warning : invalid value type "
							"for parameter \'%s = %s\'\n\tExpected "
							"type is either <int> for slave connection "
							"count or\n\t0-100%% for specifying the "
							"maximum percentage of available the "
							"slaves that will be connected.",
							param->name,
							param->value)));
					}
				}
				/** Read, validate and set max_slave_replication_lag */
				max_slave_rlag_str = config_get_value(obj->parameters, 
							"max_slave_replication_lag");

				if (max_slave_rlag_str != NULL)
				{
					CONFIG_PARAMETER* param;
					bool              succp;

					param = config_get_param(obj->parameters, 
							"max_slave_replication_lag");

					if (param == NULL)
					{
						succp = false;
					}
					else 
					{
						succp = service_set_param_value(
								service,
								param,
								max_slave_rlag_str,
								COUNT_ATMOST,
								COUNT_TYPE);
					}
							
					if (!succp)
					{
						if (param) {
							LOGIF(LM, (mxs_log(
								LOGFILE_MESSAGE,
								"* Warning : invalid value type "
								"for parameter \'%s.%s = %s\'\n\tExpected "
								"type is <int> for maximum "
								"slave replication lag.",
								((SERVICE*)obj->element)->name,
								param->name,
								param->value)));
						} else {
								LOGIF(LE, (mxs_log(
									LOGFILE_ERROR,
									"Error : parameter was NULL")));                                                                
						}
					}
				}
				if(ssl)
				{
				    if(config_handle_ssl(service,ssl,ssl_cert,ssl_key,ssl_ca_cert,ssl_version,ssl_cert_verify_depth))
				    {
					serviceDisable(service);
					service->state = SERVICE_STATE_FAILED;
				    }
				}
				else
				{
				    serviceSetSSL(service,"disabled");
				}
			}

			obj->element = service;
		}
		else
		{
			char *user;
			char *auth;
			char *enable_root_user;
			char *connection_timeout;
			char *allow_localhost_match_wildcard_host;
			char *auth_all_servers;
			char *optimize_wildcard;
			char *strip_db_esc;
			char *ssl,*ssl_cert,*ssl_key,*ssl_ca_cert,*ssl_version;
			char* ssl_cert_verify_depth;
			char* weightby;

			ssl = config_get_value(obj->parameters, "ssl");
			ssl_cert = config_get_value(obj->parameters, "ssl_cert");
			ssl_key = config_get_value(obj->parameters, "ssl_key");
			ssl_ca_cert = config_get_value(obj->parameters, "ssl_ca_cert");
			ssl_version = config_get_value(obj->parameters, "ssl_version");
			ssl_cert_verify_depth = config_get_value(obj->parameters, "ssl_cert_verify_depth");
			enable_root_user = config_get_value(obj->parameters, "enable_root_user");
			weightby = config_get_value(obj->parameters, "weightby");
			connection_timeout = config_get_value(obj->parameters, "connection_timeout");
					
			auth_all_servers = config_get_value(obj->parameters, "auth_all_servers");
			optimize_wildcard = config_get_value(obj->parameters, "optimize_wildcard");
			strip_db_esc = config_get_value(obj->parameters, "strip_db_esc");

			allow_localhost_match_wildcard_host = config_get_value(obj->parameters, "localhost_match_wildcard_host");
                    
			user = config_get_value(obj->parameters, "user");

			auth = config_get_value(obj->parameters, "passwd");

			obj->element = service_alloc(obj->object, router);
			service = obj->element;

			if(auth_all_servers)
			    serviceAuthAllServers(obj->element,config_truth_value(auth_all_servers));
			if(optimize_wildcard)
			    serviceOptimizeWildcard(obj->element,config_truth_value(optimize_wildcard));
			if(strip_db_esc)
			    serviceStripDbEsc(obj->element,config_truth_value(strip_db_esc));

			serviceWeightBy(obj->element,weightby);
			if (obj->element && user && auth)
			{
				serviceSetUser(obj->element, user, auth);

				if (enable_root_user)
					serviceEnableRootUser(obj->element, config_truth_value(enable_root_user));

				if (connection_timeout)
					serviceSetTimeout(obj->element, atoi(connection_timeout));

				if (allow_localhost_match_wildcard_host)
					serviceEnableLocalhostMatchWildcardHost(
						obj->element,
						config_truth_value(allow_localhost_match_wildcard_host));

				if(ssl)
				{
				    if(config_handle_ssl(obj->element,ssl,ssl_cert,ssl_key,ssl_ca_cert,ssl_version,ssl_cert_verify_depth))
				    {
					serviceDisable(obj->element);
					service = obj->element;
					service->state = SERVICE_STATE_FAILED;
				    }
				}
				else
				{
				    serviceSetSSL(service,"disabled");
				}
			}
			
		}
	}
	else
	{
		obj->element = NULL;
		LOGIF(LE, (mxs_log_flush(
			LOGFILE_ERROR,
			"Error : No router defined for service "
			"'%s'.",
			obj->object)));
	}
}

/*
 * Server configuration update
 * For an old server, the protocol, user, password, address and port are updated.
 * A new servers are created. Old servers which are not in the configuration context
 * will not be removed by this function.
 * @param obj		The config (type=server) object to update
 *
 */

void config_server_update(CONFIG_CONTEXT *obj) {
	SERVER *server;
	CONFIG_PARAMETER* params;
	char *address;
	char *port;
	char *protocol;
	char *monuser;
	char *monpw;

	address = config_get_value(obj->parameters, "address");
	port = config_get_value(obj->parameters, "port");
	protocol = config_get_value(obj->parameters, "protocol");
	monuser = config_get_value(obj->parameters, "monitoruser");
	monpw = config_get_value(obj->parameters, "monitorpw");

	if (address && port && protocol)
	{

		if ((server = server_find_by_unique_name(obj->object)) != NULL ||
		 (server = server_find(address, atoi(port))) != NULL)
		{
			server_update(server,
				protocol,
				monuser,
				monpw,
				 address,
				 atoi(port));
			serverClearParameters(server);
			obj->element = server;

		}
		else
		{
			obj->element = server_alloc(address,
				protocol,
				atoi(port));

			server_set_unique_name(obj->element, obj->object);

			if (obj->element && monuser && monpw)
			{
				serverAddMonUser(obj->element,
					monuser,
					monpw);
			}
		}

		params = obj->parameters;
		while (params)
		{
		    if (strcmp(params->name, "address") &&
		     strcmp(params->name, "port") &&
		     strcmp(params->name,"protocol") &&
		     strcmp(params->name,"monitoruser") &&
		     strcmp(params->name,"monitorpw") &&
		     strcmp(params->name,"type") &&
		     strcmp(params->name,"persistpoolmax") &&
		     strcmp(params->name,"persistmaxtime"))
		    {
			serverAddParameter(obj->element,
					 params->name,
					 params->value);
		    }
		    params = params->next;
		}
	}
	else
	{
		LOGIF(LE, (mxs_log_flush(
			LOGFILE_ERROR,
			"Error : Server '%s' is missing a "
			"required "
			"configuration parameter. A server must "
			"have address, port and protocol "
			"defined.",
			obj->object)));
	}
}



/*
 * Listener configuration update
 * Protocols which have not been started will be started
 * @param obj		The config (type=listener) object to update
 * @param context	The configuration data
 *
 */

void config_listener_update(CONFIG_CONTEXT *obj, CONFIG_CONTEXT *context) {
	char *service;
	char *port;
	char *protocol;
	char *address;
	char *socket;

	service = config_get_value(obj->parameters, "service");
	address = config_get_value(obj->parameters, "address");
	port = config_get_value(obj->parameters, "port");
	protocol = config_get_value(obj->parameters, "protocol");
	socket = config_get_value(obj->parameters, "socket");

	if (service && socket && protocol)
	{
		CONFIG_CONTEXT *ptr = context;
		while (ptr && strcmp(ptr->object, service) != 0)
			ptr = ptr->next;
                                
		if (ptr &&
			ptr->element &&
			serviceHasProtocol(ptr->element, protocol, 0) == 0)
		{
			serviceAddProtocol(ptr->element, protocol, socket, 0);

			serviceStartProtocol(ptr->element, protocol, 0);
		}
	}

	if (service && port && protocol)
	{
		CONFIG_CONTEXT *ptr = context;
		while (ptr && strcmp(ptr->object, service) != 0)
			ptr = ptr->next;
                                
		if (ptr &&
			ptr->element &&
			serviceHasProtocol(ptr->element, protocol, atoi(port)) == 0)
		{
			serviceAddProtocol(ptr->element,
				protocol,
				address,
				atoi(port));

			serviceStartProtocol(ptr->element,
				protocol,
				atoi(port));
		}
	}
}

/*
 * Service object update
 * This function updates backend servers, the router and filters the service uses.
 *
 * @param obj		The config (type=service) object to update
 * @param context	The configuration data
 *
 */

void config_service_update_objects(CONFIG_CONTEXT *obj, CONFIG_CONTEXT *context) {
	char *servers;
	char *roptions;
	char *filters;

	servers = config_get_value(obj->parameters, "servers");
	roptions = config_get_value(obj->parameters, "router_options");
	filters = config_get_value(obj->parameters, "filters");

	/** Only update a service which was successfully loaded */
	if(obj->element)
	{
	    if (servers)
	    {
		char *lasts;
		char *s = strtok_r(servers, ",", &lasts);
		serviceClearBackends(obj->element);
		while (s)
		{
		    CONFIG_CONTEXT *obj1 = context;
		    int		found = 0;

		    while (obj1)
		    {
			if (strcmp(trim(s), obj1->object) == 0 &&
			 obj->element && obj1->element)
			{
			    found = 1;
			    if (!serviceHasBackend(obj->element, obj1->element))
			    {
				serviceAddBackend(
					obj->element,
						 obj1->element);
			    }
			}
			obj1 = obj1->next;
		    }

		    if (!found)
		    {
			LOGIF(LE, (mxs_log_flush(
				LOGFILE_ERROR,
							 "Error: Unable to find "
				"server '%s' that is "
				"configured as part of "
				"service '%s'.",
							 s, obj->object)));
		    }
		    s = strtok_r(NULL, ",", &lasts);
		}
	    }
	    
	    /** Clear out the old router options before adding new ones */
	    serviceClearRouterOptions(obj->element);
	    if (roptions)
	    {
		char *lasts;
		char *s = strtok_r(roptions, ",", &lasts);
		while (s)
		{
		    serviceAddRouterOption(obj->element, s);
		    s = strtok_r(NULL, ",", &lasts);
		}
	    }

	    serviceUpdateFilters(obj->element,context, filters);
	    serviceUpdateRouter(obj->element, context);
	}
}

/*
 * Update a running monitor or create it if it's a new one
 *
 * @param obj		The config (type=service) object to update
 * @param context	The configuration data
 *
 */

void config_monitor_update(CONFIG_CONTEXT *obj, CONFIG_CONTEXT *context) {
	MONITOR *running;

	running = monitor_find(obj->object);

	if (running) {
		monitorStop(running);
		config_add_monitor(obj, context, running);
	} else {
		config_add_monitor(obj, context, NULL);
	}
}


/*
 * Create/Updates a monitor object
 *
 * @param obj		The config (type=service) object to update
 * @param context	The configuration data
 * @param running	Running montor to update or NULL to craete it
 *
 */

int config_add_monitor(CONFIG_CONTEXT *obj, CONFIG_CONTEXT *context, MONITOR *running) {
	int	error_count = 0;
	char	*module;
	char	*servers;
	char	*user;
	char	*passwd;
	unsigned long interval = 0;
	int connect_timeout = 0;
	int read_timeout = 0;
	int write_timeout = 0;

	module = config_get_value(obj->parameters, "module");
	servers = config_get_value(obj->parameters, "servers");
	user = config_get_value(obj->parameters, "user");
	passwd = config_get_value(obj->parameters, "passwd");

	if (config_get_value(obj->parameters, "monitor_interval")) {
		interval = strtoul(config_get_value(obj->parameters, "monitor_interval"), NULL, 10);
	}

	if (config_get_value(obj->parameters, "backend_connect_timeout")) {
		connect_timeout = atoi(config_get_value(obj->parameters, "backend_connect_timeout"));
	}
	if (config_get_value(obj->parameters, "backend_read_timeout")) {
		read_timeout = atoi(config_get_value(obj->parameters, "backend_read_timeout"));
	}
	if (config_get_value(obj->parameters, "backend_write_timeout")) {
		write_timeout = atoi(config_get_value(obj->parameters, "backend_write_timeout"));
	}
		
	if (module) {
		/* get running monitor pointer or allocate a new one */
		if (running && strcmp(running->module_name,module) == 0) {
			obj->element = running;
		} else {
			obj->element = monitor_alloc(obj->object, module);
		}
		if(obj->element == NULL)
		{
		    mxs_log(LE,"[%s] Error: Failed to load monitor module."
			    " Confirm that the module can be found in the module directory: %s",
			     obj->object,get_libdir());
		    error_count++;
		    return error_count;
		}

		if (servers) {
			char *s, *lasts;

			/* if id is not set, compute it now with pid only */
			if (gateway.id == 0) {
				gateway.id = getpid();
			}

			/* set monitor interval */
			if (interval > 0)
				monitorSetInterval(obj->element, interval);
			else
				mxs_log(LOGFILE_ERROR,"Warning: Monitor '%s' "
					    "missing monitor_interval parameter, "
					    "default value of 10000 miliseconds.",obj->object);

			/* set timeouts */
			if (connect_timeout > 0)
				monitorSetNetworkTimeout(obj->element, MONITOR_CONNECT_TIMEOUT, connect_timeout);
			if (read_timeout > 0)
				monitorSetNetworkTimeout(obj->element, MONITOR_READ_TIMEOUT, read_timeout);
			if (write_timeout > 0)
				monitorSetNetworkTimeout(obj->element, MONITOR_WRITE_TIMEOUT, write_timeout);

			/** Clear out the old servers from the monitor */
			monitorClearServers(obj->element);

			/* get the servers to monitor */
			s = strtok_r(servers, ",", &lasts);
			while (s)
			{
				CONFIG_CONTEXT *obj1 = context;
				int		found = 0;
				while (obj1)
				{
					if (strcmp(trim(s), obj1->object) == 0 &&
						obj->element && obj1->element)
					{
						found = 1;
						if(hashtable_add(monitorhash,obj1->object,"") == 0)
						{
						    mxs_log(LOGFILE_ERROR,
							"Warning: Multiple monitors are monitoring server [%s]. "
							"This will cause undefined behavior.",
							obj1->object);
						}
						if (!monitorHasBackend(obj->element, obj1->element))
                                        	{
							monitorAddServer(
								obj->element,
								obj1->element);
						}
					}
					obj1 = obj1->next;
				}
				if (!found)
					LOGIF(LE,
						(mxs_log_flush(
							LOGFILE_ERROR,
							"Error: Unable to find "
							"server '%s' that is "
							"configured in the "
							"monitor '%s'.",
							s, obj->object)));

				s = strtok_r(NULL, ",", &lasts);
			}
		}
		if (user && passwd)
		{
			monitorAddUser(obj->element,
				user,
				passwd);
		}
		else if (user)
		{
			LOGIF(LE, (mxs_log_flush(
				LOGFILE_ERROR, "Error: "
				"Monitor '%s' defines a "
				"username with no password.",
				obj->object)));
			error_count++;
		}
		monitor_set_parameters(obj->element,obj->parameters);
		((MONITOR*)obj->element)->state = MONITOR_STATE_STOPPED;
	}
	else
	{
		obj->element = NULL;
		LOGIF(LE, (mxs_log_flush(
			LOGFILE_ERROR,
			"Error : Monitor '%s' is missing a "
			"require module parameter.",
			obj->object)));
		error_count++;
	}

	return error_count;
}
/**
 * Return the pointer to the global options for MaxScale.
 * @return Pointer to the GATEWAY_CONF structure. This is a static structure and
 * should not be modified.
 */
GATEWAY_CONF* config_get_global_options()
{
    return &gateway;
}

/**
 * Check that the configuration is valid and if so, signal the threads that the
 * configuration should be reloaded.
 */
void config_set_reload_flag()
{
    if(config_verify_context())
    {
	reload_state = RELOAD_START;
    }
    else
    {
	mxs_log(LE,"Errors were found from the configuration file. Configuration reload aborted. "
		"Please fix the errors and try again.");
    }
}

/**
 * Reload a service and all modules used by it.
 * @param data Pointer to service to reload
 * @return 0 for success, -1 on error
 */
int config_reload_service(void* data)
{
    SERVICE* service = (SERVICE*)data;
    CONFIG_CONTEXT ctx;
    CONFIG_CONTEXT *ptr,*sptr;
    CONFIG_PARAMETER *param;
    char* servers = NULL;
    char* filters = NULL;
    char *saved,*tok;
    int rval = 0;
    config_read_config(&ctx);

    ptr = ctx.next;

    while(ptr && strcmp(ptr->object,service->name) != 0)
    {
	ptr = ptr->next;
    }

    if(ptr == NULL)
    {
	mxs_log(LE, "Service %s was not in the configuration file.\n",
		 service->name);
	rval = -1;
    }
    else
    {
	/** Update the servers for this service */
	param = ptr->parameters;
	while(param)
	{
	    if(strcmp(param->name,"servers") == 0)
	    {
		servers = strdup(param->value);
		break;
	    }
	    param = param->next;
	}

	if(servers)
	{
	    tok = strtok_r(servers,", ",&saved);
	    while(tok)
	    {
		sptr = ctx.next;
		while(sptr)
		{
		    if(strcmp(sptr->object,tok) == 0)
			break;
		    sptr = sptr->next;
		}
		if(sptr)
		    config_server_update(sptr);
		tok = strtok_r(NULL,", ",&saved);
	    }
	    free(servers);
	}

	/** Add new filters from the config that are used by this service */
	param = ptr->parameters;
	while(param)
	{
	    if(strcmp(param->name,"filters") == 0)
	    {
		filters = strdup(param->value);
		break;
	    }
	    param = param->next;
	}
	if(filters)
	{
	    tok = strtok_r(filters,", ",&saved);
	    while(tok)
	    {
		sptr = ctx.next;
		while(sptr)
		{
		    if(strcmp(sptr->object,tok) == 0)
			break;
		    sptr = sptr->next;
		}
		if(sptr && filter_find(sptr->object) == NULL)
		    config_add_filter(sptr);
		tok = strtok_r(NULL,", ",&saved);
	    }
	    free(filters);
	}
	config_service_update(ptr);
	config_service_update_objects(ptr,ctx.next);
	if(service->state == SERVICE_STATE_FAILED)
	    rval = -1;
	else
	    serviceRestart(service);
    }
    config_free_config(ctx.next);
    return rval;
}

/**
 * Add a filter to the configuration context. This allocates the filter module
 * and adds the filter options and parameters.
 * @param obj Configuration context
 * @return 0 on success, -1 on error
 */
int config_add_filter(CONFIG_CONTEXT* obj)
{
    int error_count = 0;
    char *module = config_get_value(obj->parameters,
				    "module");
    char *options = config_get_value(obj->parameters,
				     "options");

    if (module)
    {
	obj->element = filter_alloc(obj->object, module);
    }
    else
    {
	LOGIF(LE, (mxs_log_flush(
		LOGFILE_ERROR,"Error: Filter '%s' has no module defined to load.",
					 obj->object)));
	error_count++;
    }
    if (obj->element && options)
    {
	char *lasts;
	char *s = strtok_r(options, ",", &lasts);
	while (s)
	{
	    filterAddOption(obj->element, s);
	    s = strtok_r(NULL, ",", &lasts);
	}
    }
    if (obj->element)
    {
	CONFIG_PARAMETER *params = obj->parameters;
	while (params)
	{
	    if (strcmp(params->name, "module")
		     && strcmp(params->name,
			      "options"))
	    {
		filterAddParameter(obj->element,
				 params->name,
				 params->value);
	    }
	    params = params->next;
	}
    }
    return error_count == 0 ? 0 : -1;
}

/**
 * Return the section from the configuration context that matches the given name.
 * @param context Configuration context
 * @param name Section name to search for
 * @return Pointer to the section in the configuration or NULL if it was not found
 */
CONFIG_CONTEXT* config_get_section(CONFIG_CONTEXT* context, char* name)
{
    CONFIG_CONTEXT* section = context;

    while(section)
    {
	if(strcmp(section->object,name) == 0)
	    return section;
	section = section->next;
    }

    return NULL;
}

/**
 * Verify that all required parameters for a server are found
 * @param context Configuration context
 * @param section The server section to verify
 * @return true if the section is valid, false if errors are found
 */
bool config_verify_server(CONFIG_CONTEXT* context, CONFIG_CONTEXT* section)
{
    CONFIG_PARAMETER *port;
    int intval;
    bool rval = true;

    if((config_get_param(section->parameters,"protocol")) == NULL)
    {
	mxs_log(LE,"[%s] Error: Server is missing the 'protocol' parameter.",section->object);
	rval = false;
    }

    if((config_get_param(section->parameters,"address")) == NULL)
    {
	mxs_log(LE,"[%s] Error: Server is missing the 'address' parameter.",section->object);
	rval = false;
    }

    if((port = config_get_param(section->parameters,"port")) == NULL)
    {
	mxs_log(LE,"[%s] Error: Server is missing the 'port' parameter.",section->object);
	rval = false;
    }

    if(!config_convert_integer(port->value,&intval) || intval <= 0)
    {
	mxs_log(LE,"[%s] Error: Invalid port value: %s. ",section->object,port->value);
	rval = false;
    }

    return rval;
}

/**
 * Verify that all required parameters for a filter are found
 * @param context Configuration context
 * @param section The filter section to verify
 * @return true if the section is valid, false if errors are found
 */
bool config_verify_filter(CONFIG_CONTEXT* context, CONFIG_CONTEXT* section)
{
    bool rval = true;

    if(config_get_param(section->parameters,"module") == NULL)
    {
	mxs_log(LE,"[%s] Error: Filter is missing the 'module' parameter.",section->object);
	rval = false;
    }

    return rval;
}

/**
 * Parse a string and find the matching sections from the configuration context
 * @param context Configuration context
 * @param value A comma separated list of section names
 * @return true if all sections were found, false if a section was not found
 */
bool config_find_sections_from_string(CONFIG_CONTEXT* context,const char* needle, char* value)
{
    char *str,*tok,*sptr;
    bool rval = true;

    str = strdup(value);
    tok = strtok_r(str,needle,&sptr);
    while(tok)
    {
	if(config_get_section(context,tok) == NULL)
	{
	    mxs_log(LE,"Error: '%s' is not a valid section name.",tok);
	    rval = false;
	    break;
	}
	tok = strtok_r(NULL,needle,&sptr);
    }
    free(str);
    return rval;
}
/**
 * Verify that all required parameters for a monitor are found
 * @param context Configuration context
 * @param section The monitor section to verify
 * @return true if the section is valid, false if errors are found
 */
bool config_verify_monitor(CONFIG_CONTEXT* context, CONFIG_CONTEXT* section)
{
    bool rval = true;
    CONFIG_PARAMETER* servers;

    if((servers = config_get_param(section->parameters,"servers")) == NULL)
    {
	mxs_log(LE,"[%s] Error: Monitor is missing the 'servers' parameter.",section->object);
	rval = false;
    }
    else
    {
	if(!config_find_sections_from_string(context,", ",servers->value))
	{
	    mxs_log(LE,"[%s] Error: Monitor has invalid values in the 'servers' parameter: %s",section->object,servers->value);
	    rval = false;
	}
    }

    if(config_get_param(section->parameters,"module") == NULL)
    {
	mxs_log(LE,"[%s] Error: Monitor is missing the 'module' parameter.",section->object);
	rval = false;
    }

    if(config_get_param(section->parameters,"user") == NULL)
    {
	mxs_log(LE,"[%s] Error: Monitor is missing the 'user' parameter.",section->object);
	rval = false;
    }

    if(config_get_param(section->parameters,"passwd") == NULL)
    {
	mxs_log(LE,"[%s] Error: Monitor is missing the 'passwd' parameter.",section->object);
	rval = false;
    }

    return rval;
}

/**
 * Verify that a service section is valid and that all required parameters have been defined.
 * @param context Configuration context
 * @param section The service section of the configuration context
 * @return true if the service configuration is valid, false if one or more errors
 * were detected
 */
bool config_verify_service(CONFIG_CONTEXT* context, CONFIG_CONTEXT* section)
{
    CONFIG_CONTEXT* *search;
    CONFIG_PARAMETER *servers,*router,*filters;
    int intval;
    bool rval = true;

    if((router = config_get_param(section->parameters,"router")) == NULL)
    {
	mxs_log(LE,"[%s] Error: Service is missing the 'router' parameter.",section->object);
	rval = false;
    }

    if((servers = config_get_param(section->parameters,"servers")) == NULL)
    {
	if(router != NULL && !internalService(router->value))
	{
	    mxs_log(LE,"[%s] Error: Service is missing the 'servers' parameter.",section->object);
	    rval = false;
	}
    }
    else
    {
	if(!config_find_sections_from_string(context,", ",servers->value))
	{
	    mxs_log(LE,"[%s] Error: Service has invalid values in the 'servers' parameter: %s",section->object,servers->value);
	    rval = false;
	}
    }

    if((filters = config_get_param(section->parameters,"filters")))
    {
	if(!config_find_sections_from_string(context,"| ",filters->value))
	{
	    mxs_log(LE,"[%s] Error: Service has invalid values in the 'filters' parameter: %s",section->object,filters->value);
	    rval = false;
	}
    }

    if(router != NULL && !internalService(router->value))
    {
	if(config_get_param(section->parameters,"user") == NULL)
	{

	    mxs_log(LE,"[%s] Error: Service is missing the 'user' parameter.",section->object);
	    rval = false;
	}

	if(config_get_param(section->parameters,"passwd") == NULL)
	{
	    mxs_log(LE,"[%s] Error: Service is missing the 'passwd' parameter.",section->object);
	    rval = false;
	}
    }

    return rval;
}

/**
 * Verify that a listener section is valid and that all required parameters have been defined.
 * @param context Configuration context
 * @param section The listener section of the configuration context
 * @return true if the listener configuration is valid, false if one or more errors
 * were detected
 */
bool config_verify_listener(CONFIG_CONTEXT* context, CONFIG_CONTEXT* section)
{
    CONFIG_CONTEXT* *search;
    CONFIG_PARAMETER *service,*port,*socket,*protocol;
    int intval;
    bool rval = true;

    if((service = config_get_param(section->parameters,"service")) == NULL)
    {
	mxs_log(LE,"[%s] Error: Listener is missing the 'service' parameter.",section->object);
	rval = false;
    }

    if(config_get_section(context,service->value) == NULL)
    {
	mxs_log(LE,"[%s] Error: Service '%s' for listener not found.",
		 section->object,service->value);
	rval = false;
    }

    port = config_get_param(section->parameters,"port");
    socket = config_get_param(section->parameters,"socket");

    if(port == NULL && socket == NULL)
    {
	mxs_log(LE,"[%s] Error: Listener is missing both 'port' and 'socket' parameters. "
		"At least one of them must be defined for each listener.",
		 section->object);
	rval = false;
    }

    if(port)
    {
	if(!config_convert_integer(port->value,&intval) || intval <= 0)
	{
	    mxs_log(LE,"[%s] Error: Invalid port value: %s. ",section->object,port->value);
	    rval = false;
	}
    }

    if(socket)
    {
	if(!is_valid_posix_path(socket->value))
	{
	    mxs_log(LE,"[%s] Error: Invalid socket path: %s. ",section->object,port->value);
	    rval = false;
	}
    }

    if(config_get_param(section->parameters,"protocol") == NULL)
    {
	mxs_log(LE,"[%s] Error: Listener is missing the 'protocol' parameter.",section->object);
	rval = false;
    }

    return rval;
}

/**
 * Convert a string into an integer value and check that the conversion was valid.
 * @param value String to convert
 * @param iptr Location where the result is written
 * @return true if the string is a valid integer, otherwise false.
 */
bool config_convert_integer(char* value, int* iptr)
{
    int intval;
    char* strptr;

    intval = strtol(value,&strptr,0);

    /** No valid numbers */
    if(strptr == value)
	return false;

    /** Unexpected characters in string */
    if(*strptr != '\0')
	return false;

    *iptr = intval;

    return true;
}

/**
 * Verify the in-memory representation of the configuration file. This function
 * checks that all mandatory parameters are defined for each of the module types.
 * @param context Configuration context
 * @return true if the context is valid, false if errors were found.
 */
bool config_verify_context()
{

    CONFIG_CONTEXT base,*context,*section,*search;
    CONFIG_PARAMETER *param;
    bool rval = true;

    if (config_read_config(&base) != 0)
    {
	mxs_log(LE,"Error: Parsing the configuration file failed.");
	return false;
    }

    context = base.next;
    section = context;

    while(section)
    {
	if((param = config_get_param(section->parameters,"type")) == NULL)
	{
	    mxs_log(LE,"[%s] Error: Missing 'type' parameter.",section->object);
	    rval = false;
	}
	else
	{
	    if(strcmp(param->value,"service") == 0)
	    {
		if(!config_verify_service(context,section))
		    rval = false;
	    }
	    else if(strcmp(param->value,"listener") == 0)
	    {
		if(!config_verify_listener(context,section))
		    rval = false;
	    }
	    else if(strcmp(param->value,"monitor") == 0)
	    {
		if(!config_verify_monitor(context,section))
		    rval = false;
	    }
	    else if(strcmp(param->value,"filter") == 0)
	    {
		if(!config_verify_filter(context,section))
		    rval = false;
	    }
	    else if(strcmp(param->value,"server") == 0)
	    {
		if(!config_verify_server(context,section))
		    rval = false;
	    }
	    else
	    {
		mxs_log(LE,"[%s] Error: Invalid value for 'type' parameter. Value was not one of "
			"'server', 'listener', 'monitor', 'filter' or 'server': '%s'",section->object,param->value);
		return false;
	    }
	}
	section = section->next;
    }

    free_config_context(base.next);

    return rval;
}