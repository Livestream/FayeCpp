/*
 *   Copyright 2014 Kulykov Oleh
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */


#include "websocket.h"

#ifndef HAVE_SUITABLE_QT_VERSION

#include "../client.h"
#include "../message.h"
#include "REThread.h"
#include "REMutex.h"
#include "REBuffer.h"

#if defined(HAVE_FAYECPP_CONFIG_H)
#include "fayecpp_config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace FayeCpp {
	
	struct libwebsocket_protocols WebSocket::protocols[] = {
		{
			"default",
			WebSocket::callbackEcho,
			sizeof(EchoSessionData)
		},
		{
			NULL, NULL, 0
		}
	};
	
	void WebSocket::writeLock()
	{
		if (_writeMutex) _writeMutex->lock();
	}
	
	void WebSocket::writeUnlock()
	{
		if (_writeMutex) _writeMutex->unlock();
	}
	
	void WebSocket::onCallbackReceive(void * input, size_t len)
	{
		char * str = (char *)input;
		if (strlen(str) == len)
		{
			this->onTextReceived(std::string(str));
		}
		else
		{
			this->onDataReceived(std::vector<unsigned char>((unsigned char*)input, (unsigned char*)input + len));
		}
	}
	
	int WebSocket::callbackEcho(struct libwebsocket_context * context,
								struct libwebsocket * wsi,
								enum libwebsocket_callback_reasons reason,
								void * user,
								void * input,
								size_t len)
	{
		WebSocket * socket = static_cast<WebSocket *>(libwebsocket_context_user(context));
		EchoSessionData * pss = static_cast<EchoSessionData *>(user);
		switch (reason)
		{
			case LWS_CALLBACK_CLIENT_ESTABLISHED:
				socket->onCallbackEstablished();
				break;
				
			case LWS_CALLBACK_CLIENT_RECEIVE:
				if (input && len) socket->onCallbackReceive(input, len);
				break;
				
			case LWS_CALLBACK_CLIENT_WRITEABLE:
				return socket->onCallbackWritable(context, wsi, pss);
				break;
				
			default:
				break;
		}
		return 0;
	}
	
	void WebSocket::onCallbackEstablished()
	{
#ifdef DEBUG
		fprintf(stderr, "CALLBACK CONNECTION ESTABLISHED\n");
#endif
		this->onConnected();
	}
	
	void WebSocket::onCallbackConnectionError()
	{
#ifdef DEBUG
		fprintf(stderr, "CALLBACK CONNECTION ESTABLISHED\n");
#endif
		//TODO: error string
		this->onError("");
	}
	
	int WebSocket::onCallbackWritable(struct libwebsocket_context * context,
									  struct libwebsocket * connection,
									  EchoSessionData * pss)
	{
		REBuffer * buffer = NULL;
		
		this->writeLock();
		if (!_writeBuffers.empty())
		{
			buffer = _writeBuffers.front();
			_writeBuffers.pop_front();
		}
		this->writeUnlock();
		
		if (!buffer) return 0;
		
		const enum libwebsocket_write_protocol type = (enum libwebsocket_write_protocol)buffer->tag();
		
//		pss->len = sprintf((char *)&pss->buf[LWS_SEND_BUFFER_PRE_PADDING],
//						   "%s",
//						   (const char*)buffer->buffer());
		memcpy(&pss->buf[LWS_SEND_BUFFER_PRE_PADDING], buffer->buffer(), buffer->size());
		pss->len = buffer->size();
		
		delete buffer;
		
		const int writed = libwebsocket_write(connection, &pss->buf[LWS_SEND_BUFFER_PRE_PADDING], pss->len, type);
		if (writed < 0)
		{
#ifdef DEBUG
			fprintf(stderr, "ERROR %d writing to socket, hanging up\n", writed);
#endif
			return -1;
		}
		if (writed < pss->len)
		{
#ifdef DEBUG
			fprintf(stderr, "Partial write\n");
#endif
			return -1;
		}
		
		if (!_writeBuffers.empty())
		{
			libwebsocket_callback_on_writable(context, connection);
		}
		
		return 0;
	}
	
	void WebSocket::addWriteBufferData(const unsigned char * data, const size_t dataSize, const enum libwebsocket_write_protocol type)
	{
		bool isError = false;
		
		this->writeLock();
		
		REBuffer * buffer = new REBuffer(data, (REUInt32)dataSize);
		if (buffer && buffer->size() == dataSize)
		{
			buffer->setTag((int)type);
#ifdef DEBUG
			fprintf(stdout, "WILL WRITE: %s\n", (char *)buffer->buffer());
#endif
			_writeBuffers.push_back(buffer);
		}
		else
		{
			isError = true;
		}
		
		this->writeUnlock();
		
		if (isError)
		{
			this->onError("Can't send buffer data");
		}
		else if (this->isConnected())
		{
			libwebsocket_callback_on_writable(_context, _connection);
		}
	}
	
	void WebSocket::sendData(const std::vector<unsigned char> & data)
	{
		if (data.size() > 0)
		{
			this->addWriteBufferData(data.data(), data.size(), LWS_WRITE_BINARY);
		}
	}
	
	void WebSocket::sendText(const std::string & text)
	{
		if (text.size() > 0)
		{
			// NO NULL terminated char
			this->addWriteBufferData((const unsigned char *)text.data(), text.size(), LWS_WRITE_TEXT);
		}
	}
	
	const std::string WebSocket::name() const 
	{
		return WebSocket::transportName(); 
	}
	
	void WebSocket::connectToServer()
	{
		if (!this->isConnected())
		{
			this->start();
		}
	}
	
	void WebSocket::cleanup()
	{
		memset(&_info, 0, sizeof(struct lws_context_creation_info));
		
		if (_context)
		{
			libwebsocket_context_destroy(_context);
			_context = NULL;
		}
		_connection = NULL;
		
		for (std::list<REBuffer *>::iterator i = _writeBuffers.begin(); i != _writeBuffers.end(); ++i)
		{
			REBuffer * b = *i;
			delete b;
		}
		_writeBuffers.clear();
	}
	
	void WebSocket::disconnectFromServer()
	{
		this->cancel();
		this->cleanup();
	}
	
	void WebSocket::threadBody()
	{
		struct lws_context_creation_info info;
		memset(&info, 0, sizeof(struct lws_context_creation_info));
		
		info.port = CONTEXT_PORT_NO_LISTEN;
		info.iface = NULL;
		info.protocols = WebSocket::protocols;
		info.extensions = libwebsocket_get_internal_extensions();
		
		info.gid = -1;
		info.uid = -1;
		info.options = 0;
		info.user = static_cast<WebSocket *>(this);
		
		_context = libwebsocket_create_context(&info);
		
		if (!_context)
		{
			this->onError("Socket initialization failed");
			return;
		}
		
		_connection = libwebsocket_client_connect(_context,
												  this->host().c_str(),
												  this->port(),
												  this->isUseSSL() ? 2 : 0,
												  this->path().c_str(),
												  this->host().c_str(),
												  "origin",
												  NULL,
												  -1);
		if (!_connection)
		{
			if (_context) libwebsocket_context_destroy(_context);
			_context = NULL;
			
			char error[1024] = { 0 };
#if defined(HAVE_FUNCTION_SPRINTF_S)	
			sprintf_s(error, 1024, "Failed to connect to %s:%i", this->host().c_str(), this->port());
#else			
			sprintf(error, "Failed to connect to %s:%i", this->host().c_str(), this->port());
#endif			
			this->onError(error);
			return;
		}
		
		libwebsocket_callback_on_writable(_context, _connection);
		
		int n = 0;
		while (n >= 0 && _context /* && !force_exit */ )
		{
			n = _context ? libwebsocket_service(_context, 10) : -1;
			REThread::uSleep(10);
		}
		
		if (_context) libwebsocket_context_destroy(_context);
		_context = NULL;
	}
	
	WebSocket::WebSocket(ClassMethodWrapper<Client, void(Client::*)(Message*), Message> * processMethod) : REThread(), Transport(processMethod),
		_context(NULL),
		_connection(NULL),
		_writeMutex(new REMutex())
	{
		memset(&_info, 0, sizeof(struct lws_context_creation_info));
		
		assert(_writeMutex);
		_writeMutex->init(REMutexTypeRecursive);
		
		REThread::isMainThread();
	}
	
	WebSocket::~WebSocket()
	{
		this->cancel();
		
		if (_writeMutex)
		{
			delete _writeMutex;
			_writeMutex = NULL;
		}
		
		this->cleanup();
	}
	
	std::string WebSocket::transportName() 
	{
		return std::string("websocket");
	}
}

#endif /* #ifndef HAVE_SUITABLE_QT_VERSION */

