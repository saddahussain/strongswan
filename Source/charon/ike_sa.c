/**
 * @file ike_sa.c
 *
 * @brief Class ike_sa_t. An object of this type is managed by an
 * ike_sa_manager_t object and represents an IKE_SA
 *
 */

/*
 * Copyright (C) 2005 Jan Hutter, Martin Willi
 * Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "ike_sa.h"

#include "types.h"
#include "globals.h"
#include "utils/allocator.h"
#include "utils/linked_list.h"
#include "utils/logger_manager.h"
#include "utils/randomizer.h"
#include "payloads/sa_payload.h"
#include "payloads/nonce_payload.h"
#include "payloads/ke_payload.h"
#include "payloads/transform_substructure.h"
#include "payloads/transform_attribute.h"


/**
 * States in which a IKE_SA can actually be
 */
typedef enum ike_sa_state_e ike_sa_state_t;

enum ike_sa_state_e {

	/**
	 * IKE_SA is is not in a state
	 */
	NO_STATE = 1,

	/**
	 * A IKE_SA_INIT-message was sent: role initiator
	 */
	IKE_SA_INIT_REQUESTED = 2,

	/**
	 * A IKE_SA_INIT-message was replied: role responder
	 */
	IKE_SA_INIT_RESPONDED = 3,

	/**
	 * An IKE_AUTH-message was sent after a successful
	 * IKE_SA_INIT-exchange: role initiator
	 */
	IKE_AUTH_REQUESTED = 4,

	/**
	 * An IKE_AUTH-message was replied: role responder.
	 * In this state, all the informations for an IKE_SA
	 * and one CHILD_SA are known.
	 */
	IKE_SA_INITIALIZED = 5
};


/**
 * Private data of an message_t object
 */
typedef struct private_ike_sa_s private_ike_sa_t;

struct private_ike_sa_s {

	/**
	 * Public part of a ike_sa_t object
	 */
	ike_sa_t public;
	
	status_t (*build_sa_payload) (private_ike_sa_t *this, sa_payload_t **payload);
	status_t (*build_nonce_payload) (private_ike_sa_t *this, nonce_payload_t **payload);
	status_t (*build_ke_payload) (private_ike_sa_t *this, ke_payload_t **payload);

	/* Private values */
	/**
	 * Identifier for the current IKE_SA
	 */
	ike_sa_id_t *ike_sa_id;

	/**
	 * Linked List containing the child sa's of the current IKE_SA
	 */
	linked_list_t *child_sas;

	/**
	 * Current state of the IKE_SA
	 */
	ike_sa_state_t current_state;
	
	/**
	 * is this IKE_SA the original initiator of this IKE_SA
	 */
	bool original_initiator;
	
	/**
	 * this SA's source for random data
	 */
	randomizer_t *randomizer;
	
	linked_list_t *sent_messages;	
	
	struct {
		host_t *host;
	} me;
	
	struct {
		host_t *host;
	} other;
	
	/**
	 * a logger for this IKE_SA
	 */
	logger_t *logger;
};

/**
 * @brief implements function process_message of private_ike_sa_t
 */
static status_t process_message (private_ike_sa_t *this, message_t *message)
{
	status_t status;
	/* @TODO Add Message Processing here */
	
	this->logger->log(this->logger, CONTROL_MORE, "Process message of exchange type %s",mapping_find(exchange_type_m,message->get_exchange_type(message)));
	
	/* parse body */
	status = message->parse_body(message);
	switch (status)
	{
	 	case SUCCESS:
	 	{
	 		break;
	 	}
		default:
	 	{
	 		this->logger->log(this->logger, ERROR, "Error of type %s while parsing message body",mapping_find(status_m,status));
	 		switch (this->current_state)
	 		{
	 			case NO_STATE:
	 			{
	 				job_t *delete_job;
	 				/* create delete job for this ike_sa */
					delete_job = (job_t *) delete_ike_sa_job_create(this->ike_sa_id);
	 				if (delete_job == NULL)
	 				{
				 		this->logger->log(this->logger, ERROR, "Job to delete IKE SA could not be created");
	 				}
	 				
	 				status = global_job_queue->add(global_job_queue,delete_job);
	 				if (status != SUCCESS)
	 				{
				 		this->logger->log(this->logger, ERROR, "%s Job to delete IKE SA could not be added to job queue",mapping_find(status_m,status));
				 		delete_job->destroy_all(delete_job);
	 				}
	 				
	 			}
	 			default:
	 			{
	 				break;	
	 			}
	 		}
	 		
		 	return FAILED;
	 	}
	}
		
	return status;
}

/**
 * @brief implements function process_configuration of private_ike_sa_t
 */
static status_t initialize_connection(private_ike_sa_t *this, char *name)
{
	message_t *message;
	payload_t *payload;
	packet_t *packet;
	status_t status;
	
	this->logger->log(this->logger, CONTROL, "initializing connection");
	
	this->original_initiator = TRUE;
	
	status = global_configuration_manager->get_local_host(global_configuration_manager, name, &(this->me.host));
	if (status != SUCCESS)
	{	
		return INVALID_ARG;
	}
	
	status = global_configuration_manager->get_remote_host(global_configuration_manager, name, &(this->other.host));
	if (status != SUCCESS)
	{	
		return INVALID_ARG;
	}
	
	message = message_create();
	
	if (message == NULL)
	{
		return OUT_OF_RES;	
	}
	

	message->set_source(message, this->me.host);
	message->set_destination(message, this->other.host);

	message->set_exchange_type(message, IKE_SA_INIT);
	message->set_original_initiator(message, this->original_initiator);
	message->set_message_id(message, 0);
	message->set_ike_sa_id(message, this->ike_sa_id);
	message->set_request(message, TRUE);
	
	status = this->build_sa_payload(this, (sa_payload_t**)&payload);
	if (status != SUCCESS)
	{	
		this->logger->log(this->logger, ERROR, "Could not build SA payload");
		message->destroy(message);
		return status;
	}
	payload->set_next_type(payload, KEY_EXCHANGE);
	message->add_payload(message, payload);
	
	status = this->build_ke_payload(this, (ke_payload_t**)&payload);
	if (status != SUCCESS)
	{
		this->logger->log(this->logger, ERROR, "Could not build KE payload");
		message->destroy(message);
		return status;
	}
	payload->set_next_type(payload, NONCE);
	message->add_payload(message, payload);
	
	status = this->build_nonce_payload(this, (nonce_payload_t**)&payload);
	if (status != SUCCESS)
	{
		this->logger->log(this->logger, ERROR, "Could not build NONCE payload");
		message->destroy(message);
		return status;
	}
	payload->set_next_type(payload, NO_PAYLOAD);
	message->add_payload(message, payload);
	
	status = message->generate(message, &packet);
	if (status != SUCCESS)
	{
		this->logger->log(this->logger, ERROR, "Could not generate message");
		message->destroy(message);
		return status;
	}
	
	
	global_send_queue->add(global_send_queue, packet);

	message->destroy(message);

	this->current_state = IKE_SA_INIT_REQUESTED;

	return SUCCESS;
}

/**
 * @brief implements function private_ike_sa_t.get_id
 */
static ike_sa_id_t* get_id(private_ike_sa_t *this)
{
	return this->ike_sa_id;
}

/**
 * implements private_ike_sa_t.build_sa_payload
 */
static status_t build_sa_payload(private_ike_sa_t *this, sa_payload_t **payload)
{
	sa_payload_t* sa_payload;
	linked_list_iterator_t *iterator;
	status_t status;

	this->logger->log(this->logger, CONTROL_MORE, "building sa payload");
	
	sa_payload = sa_payload_create();
	if (sa_payload == NULL)
	{
		return OUT_OF_RES;
	}
	status = sa_payload->create_proposal_substructure_iterator(sa_payload, &iterator, FALSE);
	if (status != SUCCESS)
	{
		sa_payload->destroy(sa_payload);
		return status;
	}
	status = global_configuration_manager->get_proposals_for_host(global_configuration_manager, this->other.host, iterator);
	if (status != SUCCESS)
	{
		sa_payload->destroy(sa_payload);
		return status;
	}
	
	*payload = sa_payload;
	
	return SUCCESS;
}

/**
 * implements private_ike_sa_t.build_ke_payload
 */
static status_t build_ke_payload(private_ike_sa_t *this, ke_payload_t **payload)
{
	ke_payload_t *ke_payload;
	chunk_t key_data;
	
	
	this->logger->log(this->logger, CONTROL_MORE, "building ke payload");
	
	key_data.ptr = "12345";
	key_data.len = strlen("12345");
	
	
	ke_payload = ke_payload_create();
	if (ke_payload == NULL)
	{
		return OUT_OF_RES;	
	}
	ke_payload->set_dh_group_number(ke_payload, MODP_1024_BIT);
	if (ke_payload->set_key_exchange_data(ke_payload, key_data) != SUCCESS)
	{
		ke_payload->destroy(ke_payload);
		return OUT_OF_RES;
	}
	*payload = ke_payload;
	return SUCCESS;
}

/**
 * implements private_ike_sa_t.build_nonce_payload
 */
static status_t build_nonce_payload(private_ike_sa_t *this, nonce_payload_t **payload)
{
	nonce_payload_t *nonce_payload;
	chunk_t nonce;
	
	this->logger->log(this->logger, CONTROL_MORE, "building nonce payload");
	
	if (this->randomizer->allocate_pseudo_random_bytes(this->randomizer, 16, &nonce) != SUCCESS)
	{
		return OUT_OF_RES;
	}
	
	nonce_payload = nonce_payload_create();
	if (nonce_payload == NULL)
	{
		return OUT_OF_RES;	
	}
	
	nonce_payload->set_nonce(nonce_payload, nonce);
	
	*payload = nonce_payload;
	
	return SUCCESS;
}

/**
 * @brief implements function destroy of private_ike_sa_t
 */
static status_t destroy (private_ike_sa_t *this)
{
	linked_list_iterator_t *iterator;

	this->child_sas->create_iterator(this->child_sas, &iterator, TRUE);
	while (iterator->has_next(iterator))
	{
		payload_t *payload;
		iterator->current(iterator, (void**)&payload);
		payload->destroy(payload);
	}
	iterator->destroy(iterator);
	this->child_sas->destroy(this->child_sas);
	
	this->ike_sa_id->destroy(this->ike_sa_id);
	this->sent_messages->destroy(this->sent_messages);
	this->randomizer->destroy(this->randomizer);
	
	global_logger_manager->destroy_logger(global_logger_manager, this->logger);

	allocator_free(this);

	return SUCCESS;
}

/*
 * Described in Header
 */
ike_sa_t * ike_sa_create(ike_sa_id_t *ike_sa_id)
{
	private_ike_sa_t *this = allocator_alloc_thing(private_ike_sa_t);
	if (this == NULL)
	{
		return NULL;
	}


	/* Public functions */
	this->public.process_message = (status_t(*)(ike_sa_t*, message_t*)) process_message;
	this->public.initialize_connection = (status_t(*)(ike_sa_t*, char*)) initialize_connection;
	this->public.get_id = (ike_sa_id_t*(*)(ike_sa_t*)) get_id;
	this->public.destroy = (status_t(*)(ike_sa_t*))destroy;
	
	this->build_sa_payload = build_sa_payload;
	this->build_ke_payload = build_ke_payload;
	this->build_nonce_payload = build_nonce_payload;



	/* initialize private fields */
	if (ike_sa_id->clone(ike_sa_id,&(this->ike_sa_id)) != SUCCESS)
	{
		allocator_free(this);
		return NULL;
	}
	this->child_sas = linked_list_create();
	if (this->child_sas == NULL)
	{
		this->ike_sa_id->destroy(this->ike_sa_id);
		allocator_free(this);
		return NULL;
	}
	this->randomizer = randomizer_create();
	if (this->randomizer == NULL)
	{
		this->child_sas->destroy(this->child_sas);
		this->ike_sa_id->destroy(this->ike_sa_id);
		allocator_free(this);
	}
	this->sent_messages = linked_list_create();
	if (this->sent_messages == NULL)
	{
		this->randomizer->destroy(this->randomizer);
		this->child_sas->destroy(this->child_sas);
		this->ike_sa_id->destroy(this->ike_sa_id);
		allocator_free(this);
	}
	this->logger = global_logger_manager->create_logger(global_logger_manager, IKE_SA, NULL);
	if (this->logger ==  NULL)
	{
		this->randomizer->destroy(this->randomizer);
		this->child_sas->destroy(this->child_sas);
		this->ike_sa_id->destroy(this->ike_sa_id);
		this->sent_messages->destroy(this->sent_messages);
		allocator_free(this);
	}
	
	this->me.host = NULL;
	this->other.host = NULL;


	/* at creation time, IKE_SA isn't in a specific state */
	this->current_state = NO_STATE;

	return (&this->public);
}
