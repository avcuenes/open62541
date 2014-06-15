/*
 * ua_stack_session.c
 *
 *  Created on: 05.06.2014
 *      Author: root
 */


#include "ua_stack_session.h"
typedef struct UA_SessionType
{
	UA_NodeId authenticationToken;
	UA_NodeId sessionId;
	UA_String name;
	void *applicationPayload;
	Application *application;
	UA_list_List pendingRequests;
	SL_secureChannel channel;
	UA_UInt32 maxRequestMessageSize;
	UA_UInt32 maxResponseMessageSize;

}UA_SessionType;

UA_Int32 UA_Session_new(UA_Session **newSession)
{
	UA_Int32 retval = UA_SUCCESS;
	UA_Session *session;

	retval |= UA_alloc((void**)&session,sizeof(UA_Session));

	retval |= UA_alloc((void**)session,sizeof(UA_SessionType));
	*newSession = session;
	**newSession = *session;
	//get memory for request list
	return retval;
}

UA_Int32 UA_Session_init(UA_Session session, UA_String *sessionName, UA_Double requestedSessionTimeout, UA_UInt32 maxRequestMessageSize, UA_UInt32 maxResponseMessageSize)
{
	UA_Int32 retval = UA_SUCCESS;
	retval |= UA_String_copy(sessionName, &((UA_SessionType*)session)->name);
	((UA_SessionType*)session)->maxRequestMessageSize = maxRequestMessageSize;
	((UA_SessionType*)session)->maxResponseMessageSize = maxResponseMessageSize;

	//TODO handle requestedSessionTimeout
	return retval;
}

UA_Boolean UA_Session_compare(UA_Session session1, UA_Session session2)
{
	if(session1 && session2){
		return UA_NodeId_compare(&((UA_SessionType*)session1)->sessionId,
				&((UA_SessionType*)session2)->sessionId) == 0;
	}
	return UA_FALSE;
}

UA_Boolean UA_Session_compareByToken(UA_Session session, UA_NodeId *token)
{
	if(session && token){
		return UA_NodeId_compare(&((UA_SessionType*)session)->authenticationToken, token);
	}
	return UA_FALSE;
}

UA_Boolean UA_Session_compareById(UA_Session session, UA_NodeId *sessionId)
{
	if(session && sessionId){
		return UA_NodeId_compare(&((UA_SessionType*)session)->sessionId, sessionId);
	}
	return UA_FALSE;
}

UA_Int32 UA_Session_getId(UA_Session session, UA_NodeId *sessionId)
{
	if(session)
	{
		return UA_NodeId_copy(&((UA_SessionType*)session)->sessionId, sessionId);
	}
	return UA_ERROR;
}

UA_Int32 UA_Session_getChannel(UA_Session session, SL_secureChannel *channel)
{
	if(session)
	{
		*channel = ((UA_SessionType*)session)->channel;
		return UA_SUCCESS;
	}
	return UA_ERROR;

}
