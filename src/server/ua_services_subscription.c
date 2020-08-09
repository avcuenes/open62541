/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. 
 *
 *    Copyright 2014-2018 (c) Fraunhofer IOSB (Author: Julius Pfrommer)
 *    Copyright 2016-2017 (c) Florian Palm
 *    Copyright 2015 (c) Chris Iatrou
 *    Copyright 2015-2016 (c) Sten Grüner
 *    Copyright 2015-2016 (c) Oleksiy Vasylyev
 *    Copyright 2017 (c) Stefan Profanter, fortiss GmbH
 *    Copyright 2018 (c) Ari Breitkreuz, fortiss GmbH
 *    Copyright 2017 (c) Mattias Bornhager
 *    Copyright 2017 (c) Henrik Norrman
 *    Copyright 2017-2018 (c) Thomas Stalder, Blue Time Concept SA
 *    Copyright 2018 (c) Fabian Arndt, Root-Core
 *    Copyright 2017-2019 (c) HMS Industrial Networks AB (Author: Jonas Green)
 */

#include "ua_server_internal.h"
#include "ua_services.h"
#include "ua_subscription.h"

#ifdef UA_ENABLE_SUBSCRIPTIONS /* conditional compilation */

static void
setSubscriptionSettings(UA_Server *server, UA_Subscription *subscription,
                        UA_Double requestedPublishingInterval,
                        UA_UInt32 requestedLifetimeCount,
                        UA_UInt32 requestedMaxKeepAliveCount,
                        UA_UInt32 maxNotificationsPerPublish,
                        UA_Byte priority) {
    UA_LOCK_ASSERT(server->serviceMutex, 1);

    /* re-parameterize the subscription */
    UA_BOUNDEDVALUE_SETWBOUNDS(server->config.publishingIntervalLimits,
                               requestedPublishingInterval, subscription->publishingInterval);
    /* check for nan*/
    if(requestedPublishingInterval != requestedPublishingInterval)
        subscription->publishingInterval = server->config.publishingIntervalLimits.min;
    UA_BOUNDEDVALUE_SETWBOUNDS(server->config.keepAliveCountLimits,
                               requestedMaxKeepAliveCount, subscription->maxKeepAliveCount);
    UA_BOUNDEDVALUE_SETWBOUNDS(server->config.lifeTimeCountLimits,
                               requestedLifetimeCount, subscription->lifeTimeCount);
    if(subscription->lifeTimeCount < 3 * subscription->maxKeepAliveCount)
        subscription->lifeTimeCount = 3 * subscription->maxKeepAliveCount;
    subscription->notificationsPerPublish = maxNotificationsPerPublish;
    if(maxNotificationsPerPublish == 0 ||
       maxNotificationsPerPublish > server->config.maxNotificationsPerPublish)
        subscription->notificationsPerPublish = server->config.maxNotificationsPerPublish;
    subscription->priority = priority;
}

void
Service_CreateSubscription(UA_Server *server, UA_Session *session,
                           const UA_CreateSubscriptionRequest *request,
                           UA_CreateSubscriptionResponse *response) {
    UA_LOCK_ASSERT(server->serviceMutex, 1);

    /* Check limits for the number of subscriptions */
    if(((server->config.maxSubscriptions != 0) &&
        (server->numSubscriptions >= server->config.maxSubscriptions)) ||
       ((server->config.maxSubscriptionsPerSession != 0) &&
        (session->numSubscriptions >= server->config.maxSubscriptionsPerSession))) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADTOOMANYSUBSCRIPTIONS;
        return;
    }

    /* Create the subscription */
    UA_Subscription *sub= UA_Subscription_new();
    if(!sub) {
        UA_LOG_DEBUG_SESSION(&server->config.logger, session,
                             "Processing CreateSubscriptionRequest failed");
        response->responseHeader.serviceResult = UA_STATUSCODE_BADOUTOFMEMORY;
        return;
    }

    /* Set the subscription parameters */
    setSubscriptionSettings(server, sub, request->requestedPublishingInterval,
                            request->requestedLifetimeCount, request->requestedMaxKeepAliveCount,
                            request->maxNotificationsPerPublish, request->priority);
    sub->publishingEnabled = request->publishingEnabled;
    sub->currentKeepAliveCount = sub->maxKeepAliveCount; /* set settings first */

    UA_StatusCode retval = Subscription_registerPublishCallback(server, sub);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_DEBUG_SESSION(&server->config.logger, sub->session,
                             "Subscription %" PRIu32 " | "
                             "Could not register publish callback with error code %s",
                             sub->subscriptionId, UA_StatusCode_name(retval));
        response->responseHeader.serviceResult = retval;
        UA_free(sub);
        return;
    }

    /* Also assigns the SubscriptionId */
    UA_Server_addSubscription(server, session, sub);

    /* Prepare the response */
    response->subscriptionId = sub->subscriptionId;
    response->revisedPublishingInterval = sub->publishingInterval;
    response->revisedLifetimeCount = sub->lifeTimeCount;
    response->revisedMaxKeepAliveCount = sub->maxKeepAliveCount;

    UA_LOG_INFO_SESSION(&server->config.logger, session, "Subscription %" PRIu32 " | "
                        "Created the Subscription with a publishing interval of %.2f ms",
                        response->subscriptionId, sub->publishingInterval);
}

void
Service_ModifySubscription(UA_Server *server, UA_Session *session,
                           const UA_ModifySubscriptionRequest *request,
                           UA_ModifySubscriptionResponse *response) {
    UA_LOG_DEBUG_SESSION(&server->config.logger, session, "Processing ModifySubscriptionRequest");
    UA_LOCK_ASSERT(server->serviceMutex, 1);

    UA_Subscription *sub = UA_Session_getSubscriptionById(session, request->subscriptionId);
    if(!sub) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADSUBSCRIPTIONIDINVALID;
        return;
    }

    /* Store the old publishing interval */
    UA_Double oldPublishingInterval = sub->publishingInterval;

    /* Change the Subscription settings */
    setSubscriptionSettings(server, sub, request->requestedPublishingInterval,
                            request->requestedLifetimeCount, request->requestedMaxKeepAliveCount,
                            request->maxNotificationsPerPublish, request->priority);

    /* Reset the subscription lifetime */
    sub->currentLifetimeCount = 0;

    /* Change the repeated callback to the new interval. This cannot fail as the
     * CallbackId must exist. */
    if(sub->publishCallbackIsRegistered &&
       sub->publishingInterval != oldPublishingInterval)
        changeRepeatedCallbackInterval(server, sub->publishCallbackId, sub->publishingInterval);

    /* Set the response */
    response->revisedPublishingInterval = sub->publishingInterval;
    response->revisedLifetimeCount = sub->lifeTimeCount;
    response->revisedMaxKeepAliveCount = sub->maxKeepAliveCount;
}

static void
Operation_SetPublishingMode(UA_Server *server, UA_Session *session,
                            const UA_Boolean *publishingEnabled, const UA_UInt32 *subscriptionId,
                            UA_StatusCode *result) {
    UA_LOCK_ASSERT(server->serviceMutex, 1);
    UA_Subscription *sub = UA_Session_getSubscriptionById(session, *subscriptionId);
    if(!sub) {
        *result = UA_STATUSCODE_BADSUBSCRIPTIONIDINVALID;
        return;
    }

    sub->currentLifetimeCount = 0; /* Reset the subscription lifetime */
    sub->publishingEnabled = *publishingEnabled; /* Set the publishing mode */
}

void
Service_SetPublishingMode(UA_Server *server, UA_Session *session,
                          const UA_SetPublishingModeRequest *request,
                          UA_SetPublishingModeResponse *response) {
    UA_LOG_DEBUG_SESSION(&server->config.logger, session, "Processing SetPublishingModeRequest");
    UA_LOCK_ASSERT(server->serviceMutex, 1);

    UA_Boolean publishingEnabled = request->publishingEnabled; /* request is const */
    response->responseHeader.serviceResult =
        UA_Server_processServiceOperations(server, session, (UA_ServiceOperation)Operation_SetPublishingMode,
                                           &publishingEnabled,
                                           &request->subscriptionIdsSize, &UA_TYPES[UA_TYPES_UINT32],
                                           &response->resultsSize, &UA_TYPES[UA_TYPES_STATUSCODE]);
}

void
Service_Publish(UA_Server *server, UA_Session *session,
                const UA_PublishRequest *request, UA_UInt32 requestId) {
    UA_LOG_DEBUG_SESSION(&server->config.logger, session, "Processing PublishRequest");
    UA_LOCK_ASSERT(server->serviceMutex, 1);

    /* Return an error if the session has no subscription */
    if(TAILQ_EMPTY(&session->subscriptions)) {
        sendServiceFault(session->header.channel, requestId, request->requestHeader.requestHandle,
                         &UA_TYPES[UA_TYPES_PUBLISHRESPONSE], UA_STATUSCODE_BADNOSUBSCRIPTION);
        return;
    }

    /* Handle too many subscriptions to free resources before trying to allocate
     * resources for the new publish request. If the limit has been reached the
     * oldest publish request shall be responded */
    if((server->config.maxPublishReqPerSession != 0) &&
       (session->numPublishReq >= server->config.maxPublishReqPerSession)) {
        if(!UA_Session_reachedPublishReqLimit(server, session)) {
            sendServiceFault(session->header.channel, requestId, request->requestHeader.requestHandle,
                             &UA_TYPES[UA_TYPES_PUBLISHRESPONSE], UA_STATUSCODE_BADINTERNALERROR);
            return;
        }
    }

    /* Allocate the response to store it in the retransmission queue */
    UA_PublishResponseEntry *entry = (UA_PublishResponseEntry *)
        UA_malloc(sizeof(UA_PublishResponseEntry));
    if(!entry) {
        sendServiceFault(session->header.channel, requestId, request->requestHeader.requestHandle,
                         &UA_TYPES[UA_TYPES_PUBLISHRESPONSE], UA_STATUSCODE_BADOUTOFMEMORY);
        return;
    }

    /* Prepare the response */
    entry->requestId = requestId;
    UA_PublishResponse *response = &entry->response;
    UA_PublishResponse_init(response);
    response->responseHeader.requestHandle = request->requestHeader.requestHandle;

    /* Allocate the results array to acknowledge the acknowledge */
    if(request->subscriptionAcknowledgementsSize > 0) {
        response->results = (UA_StatusCode *)
            UA_Array_new(request->subscriptionAcknowledgementsSize,
                         &UA_TYPES[UA_TYPES_STATUSCODE]);
        if(!response->results) {
            UA_free(entry);
            sendServiceFault(session->header.channel, requestId, request->requestHeader.requestHandle,
                             &UA_TYPES[UA_TYPES_PUBLISHRESPONSE], UA_STATUSCODE_BADOUTOFMEMORY);
            return;
        }
        response->resultsSize = request->subscriptionAcknowledgementsSize;
    }

    /* Delete Acknowledged Subscription Messages */
    for(size_t i = 0; i < request->subscriptionAcknowledgementsSize; ++i) {
        UA_SubscriptionAcknowledgement *ack = &request->subscriptionAcknowledgements[i];
        UA_Subscription *sub = UA_Session_getSubscriptionById(session, ack->subscriptionId);
        if(!sub) {
            response->results[i] = UA_STATUSCODE_BADSUBSCRIPTIONIDINVALID;
            UA_LOG_DEBUG_SESSION(&server->config.logger, session,
                                 "Cannot process acknowledgements subscription %u" PRIu32,
                                 ack->subscriptionId);
            continue;
        }
        /* Remove the acked transmission from the retransmission queue */
        response->results[i] = UA_Subscription_removeRetransmissionMessage(sub, ack->sequenceNumber);
    }

    /* Queue the publish response. It will be dequeued in a repeated publish
     * callback. This can also be triggered right now for a late
     * subscription. */
    UA_Session_queuePublishReq(session, entry, false);
    UA_LOG_DEBUG_SESSION(&server->config.logger, session, "Queued a publication message");

    /* If there are late subscriptions, the new publish request is used to
     * answer them immediately. However, a single subscription that generates
     * many notifications must not "starve" other late subscriptions. Hence we
     * move it to the end of the queue when a response was sent. */
    UA_Subscription *late = NULL;
    TAILQ_FOREACH(late, &session->subscriptions, sessionListEntry) {
        if(late->state != UA_SUBSCRIPTIONSTATE_LATE)
            continue;

        UA_LOG_DEBUG_SUBSCRIPTION(&server->config.logger, late,
                                  "Send PublishResponse on a late subscription");
        UA_Subscription_publish(server, late);
        /* If the subscription was not detached from the session during publish,
         * enqueue at the end */
        if(late->session) {
            TAILQ_REMOVE(&session->subscriptions, late, sessionListEntry);
            TAILQ_INSERT_TAIL(&session->subscriptions, late, sessionListEntry);
        }
        break;
    }
}

static void
Operation_DeleteSubscription(UA_Server *server, UA_Session *session, void *_,
                             const UA_UInt32 *subscriptionId, UA_StatusCode *result) {
    UA_Subscription *sub = UA_Session_getSubscriptionById(session, *subscriptionId);
    if(!sub) {
        *result = UA_STATUSCODE_BADSUBSCRIPTIONIDINVALID;
        UA_LOG_DEBUG_SESSION(&server->config.logger, session,
                             "Deleting Subscription with Id %" PRIu32 " failed with error code %s",
                             *subscriptionId, UA_StatusCode_name(*result));
        return;
    }

    UA_Server_deleteSubscription(server, sub);
    *result = UA_STATUSCODE_GOOD;
    UA_LOG_DEBUG_SESSION(&server->config.logger, session,
                         "Subscription %" PRIu32 " | Subscription deleted",
                         *subscriptionId);
}

void
Service_DeleteSubscriptions(UA_Server *server, UA_Session *session,
                            const UA_DeleteSubscriptionsRequest *request,
                            UA_DeleteSubscriptionsResponse *response) {
    UA_LOG_DEBUG_SESSION(&server->config.logger, session,
                         "Processing DeleteSubscriptionsRequest");
    UA_LOCK_ASSERT(server->serviceMutex, 1);

    response->responseHeader.serviceResult =
        UA_Server_processServiceOperations(server, session,
                  (UA_ServiceOperation)Operation_DeleteSubscription, NULL,
                  &request->subscriptionIdsSize, &UA_TYPES[UA_TYPES_UINT32],
                  &response->resultsSize, &UA_TYPES[UA_TYPES_STATUSCODE]);
}

void
Service_Republish(UA_Server *server, UA_Session *session,
                  const UA_RepublishRequest *request,
                  UA_RepublishResponse *response) {
    UA_LOG_DEBUG_SESSION(&server->config.logger, session,
                         "Processing RepublishRequest");
    UA_LOCK_ASSERT(server->serviceMutex, 1);

    /* Get the subscription */
    UA_Subscription *sub = UA_Session_getSubscriptionById(session, request->subscriptionId);
    if(!sub) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADSUBSCRIPTIONIDINVALID;
        return;
    }

    /* Reset the subscription lifetime */
    sub->currentLifetimeCount = 0;

    /* Find the notification in the retransmission queue  */
    UA_NotificationMessageEntry *entry;
    TAILQ_FOREACH(entry, &sub->retransmissionQueue, listEntry) {
        if(entry->message.sequenceNumber == request->retransmitSequenceNumber)
            break;
    }
    if(!entry) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADMESSAGENOTAVAILABLE;
        return;
    }

    response->responseHeader.serviceResult =
        UA_NotificationMessage_copy(&entry->message, &response->notificationMessage);
}

#endif /* UA_ENABLE_SUBSCRIPTIONS */
