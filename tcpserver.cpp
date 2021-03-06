#include "tcpserver.h"


#define WRITE_QUEUE_MAX_SIZE	1000000
#define RECEIVE_BUFFER_SIZE 	4096
#define MAX_RECEIVE_BUFFER  	4096000

static void __SetNonblock(int fd)
{
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static void __SetReuseaddr(int fd)
{
	int reuse = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
}

static void __SetNodelay(int fd)
{
	int nodelay = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
}


//====================CTcpConnection================================

CTcpConnection::CTcpConnection(CTcpServer * pTcpServer, const SocketClientData_t & sClient) 
{
	m_pTcpServer = pTcpServer;
	m_SocketClient = sClient;
	m_Socket = sClient.Socket;

	__SetNonblock(m_SocketClient.Socket);
	__SetReuseaddr(m_SocketClient.Socket);
	__SetNodelay(m_SocketClient.Socket);

	m_Io.set(m_pTcpServer->GetLoop());
	m_Timer.set(m_pTcpServer->GetLoop());

	m_Io.set<CTcpConnection, &CTcpConnection::__IoCallback>(this);
	m_Io.start(m_SocketClient.Socket, ev::READ);

	m_Timer.set<CTcpConnection, &CTcpConnection::__TimerCallback>(this);
	m_Timer.start(ev::tstamp(m_pTcpServer->GetConnectionTimeout()), m_pTcpServer->GetConnectionTimeout());

	m_nLasttime = (int)time(0);

}

CTcpConnection::~CTcpConnection() 
{
	Close();
	
	m_Io.stop();
	m_Timer.stop();
}

void CTcpConnection::Send(COutputBuffer::Pointer pOutputBuffer)
{
	if (m_WriteQueue.size() > WRITE_QUEUE_MAX_SIZE)
	{
		m_pTcpServer->OnClientSendError(m_SocketClient, 0);
		return;
	}
	
	m_WriteQueue.push(pOutputBuffer);
	
	m_Io.set(ev::READ|ev::WRITE);
}

void CTcpConnection::Close()
{
	if (m_Socket > 0)
	{
		shutdown(m_Socket, SHUT_RDWR);
		close(m_Socket);

		m_Socket = 0;
	}
}

void CTcpConnection::__IoCallback(ev::io &watcher, int revents) 
{
	if (EV_ERROR & revents) 
	{
		__ErrorCallback();
		return;
	}

	if (revents & EV_READ)
	{
		char *szBuffer = (char *)malloc(RECEIVE_BUFFER_SIZE*2);
		ssize_t nRead = 0;
		
		unsigned int total_read = RECEIVE_BUFFER_SIZE;
		unsigned int current_read = 0;
		unsigned int has_get_len = 0;
		
		struct sSubmitData
		{
			unsigned int cLen;
			void *cData(){return this+1};
		};
		
		do
		{
			int start_pos = current_read;
			int read_size = (current_read+RECEIVE_BUFFER_SIZE)>total_read?(total_read-current_read):RECEIVE_BUFFER_SIZE;
			ssize_t tRead = recv(watcher.fd, szBuffer+start_pos, read_size, 0);
			
			if(tRead == 0)
			{
				break;
			}else if(tRead < 0)
			{
				if(errno == EAGAIN)
				{
					continue;
				}else
				{
					break;
				}
			}
			
			nRead += tRead;
			current_read += tRead;
			if( (0 == has_get_len) && (nRead >= sizeof(sSubmitData)) )
			{
				sSubmitData* p_sub = (sSubmitData*)szBuffer;
				size_t total_len = sizeof(sSubmitData)+p_sub->cLen;
				total_read = total_len;
				int total_block = (total_len+RECEIVE_BUFFER_SIZE-1)/RECEIVE_BUFFER_SIZE;
				
				size_t buffer_size = total_block*RECEIVE_BUFFER_SIZE;
				if(buffer_size >= MAX_RECEIVE_BUFFER)
				{
					break;
				}
				else if(buffer_size >= RECEIVE_BUFFER_SIZE*2)
				{
					char *p_buf = szBuffer;
					szBuffer = (char *)malloc(buffer_size);
					memcpy(szBuffer, p_buf, nRead);
					free(p_buf);
				}	
				has_get_len = 1;
			}
		}while(current_read<total_read);
		
		
		if (nRead == 0) 
		{
			m_pTcpServer->OnClientDisconnected(m_SocketClient, 0);
			return;
		} 
		else if (nRead < 0) 
		{
			if (errno == EAGAIN)
			{
				// ignore	
			}
			else
			{
			
				m_pTcpServer->OnClientRecvError(m_SocketClient, errno);
				return;
			}
		
			
		}else
		{
			m_nLasttime = (int)time(0);
			m_pTcpServer->OnClientDataReceived(m_SocketClient, szBuffer, nRead);
		}
		
		free(szBuffer);
	}

	if (revents & EV_WRITE)
	{
		if (m_WriteQueue.empty()) 
		{
			m_Io.set(ev::READ);
			return;
		}

		COutputBuffer::Pointer pBuffer = m_WriteQueue.front();

		ssize_t nWritten = write(watcher.fd, pBuffer->GetDataPos(), pBuffer->GetBytes());
		if (nWritten < 0) 
		{
			if (errno == EAGAIN)
			{
				// ignore	
				return;
			}
			else
			{
				m_pTcpServer->OnClientSendError(m_SocketClient, errno);
				return;
			}
		}

		m_nLasttime = (int)time(0);

		pBuffer->m_nPos += nWritten;
		if (pBuffer->GetBytes() == 0) 
		{
			m_WriteQueue.pop();
		}

		if (m_WriteQueue.empty()) 
		{
			m_Io.set(ev::READ);
		}
	}
}

void CTcpConnection::__TimerCallback(ev::timer &watcher, int revents)
{
	if (EV_ERROR & revents) 
	{
		__ErrorCallback();
		return;
	}

	if (revents & EV_TIMER)
	{
		if((int)time(0) - m_nLasttime >= TCP_CONNECTION_TIMEOUT)
		{
			m_pTcpServer->OnClientTimeout(m_SocketClient);
		}
	}
}

void CTcpConnection::__ErrorCallback()
{
	m_pTcpServer->OnClientDisconnected(m_SocketClient, errno);
}


//====================CTcpServer================================

bool CTcpServer::Start(unsigned int IP, unsigned short Port)
{
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(Port);
	addr.sin_addr.s_addr = htonl(IP);

	m_Socket = socket(PF_INET, SOCK_STREAM, 0);

	__SetNonblock(m_Socket);
	__SetReuseaddr(m_Socket);

	if (bind(m_Socket, (struct sockaddr *)&addr, sizeof(addr)) != 0) 
	{
		// bind error
		return false;
	}

	listen(m_Socket, SOMAXCONN);

	m_ListenIo.set(m_Loop);
	m_Async.set(m_Loop);

	m_ListenIo.set<CTcpServer, &CTcpServer::__Accept>(this);
	m_ListenIo.start(m_Socket, ev::READ);

	m_Async.set<CTcpServer, &CTcpServer::__AsyncCallback>(this);
	m_Async.start();
	
	CSigHandle::GetInstance(m_Loop);
	
	m_Loop.loop();

	return true;
}

bool CTcpServer::Stop()
{
	shutdown(m_Socket, SHUT_RDWR);
	close(m_Socket);

	m_ListenIo.stop();
	m_Async.stop();
	m_Loop.unloop();

	return true;
}

bool CTcpServer::Send(SocketClientData_t sClient, const char *pData, int nDataLen)
{
	pthread_spin_lock(&m_Spinlock);

	COutputBuffer::Pointer pOutputBuffer(new COutputBuffer(pData, nDataLen));
	m_Functions.push_back(std::tr1::bind(&CTcpServer::__Send, this, sClient, pOutputBuffer));
	m_Async.send();
	
	pthread_spin_unlock(&m_Spinlock);
	
	return true;
}

bool CTcpServer::CloseClient(SocketClientData_t sClient)
{
	CTcpConnection::Pointer pTcpConnection = m_SocketInfoManager.Get(sClient);
	if (pTcpConnection)
	{
		pTcpConnection->Close();
	}

	return true;
}

void CTcpServer::OnClientDisconnected(SocketClientData_t sClient, int nErrorCode)
{
	m_pDataHandle->OnClientDisconnected(sClient, nErrorCode);

	m_SocketInfoManager.Remove(sClient);
}

void CTcpServer::OnClientDataReceived(SocketClientData_t sClient, const char * pData, int nDataLen)
{
	m_pDataHandle->OnClientDataReceived(sClient, pData, nDataLen);
}

void CTcpServer::OnClientRecvError(SocketClientData_t sClient, int nErrorCode)
{
	m_pDataHandle->OnClientDisconnected(sClient, nErrorCode);

	m_SocketInfoManager.Remove(sClient);
}

void CTcpServer::OnClientSendError(SocketClientData_t sClient, int nErrorCode)
{
	m_pDataHandle->OnClientDisconnected(sClient, nErrorCode);

	m_SocketInfoManager.Remove(sClient);
}

void  CTcpServer::OnClientTimeout(SocketClientData_t sClient)
{
	m_pDataHandle->OnClientDisconnected(sClient, 0);

	m_SocketInfoManager.Remove(sClient);
}

bool CTcpServer::__Send(SocketClientData_t sClient, COutputBuffer::Pointer pOutputBuffer)
{
	CTcpConnection::Pointer pTcpConnection = m_SocketInfoManager.Get(sClient);
	if (pTcpConnection)
	{
		pTcpConnection->Send(pOutputBuffer);
		return true;
	}

	return false;
}

void CTcpServer::__Accept(ev::io &watcher, int revents) 
{
	if (EV_ERROR & revents) 
	{
		return;
	}

	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(addr);

	int client_fd = accept(watcher.fd, (struct sockaddr *)&addr, &addrlen);

	if (client_fd < 0) 
	{
		return;
	}

	SocketClientData_t sClient = {ntohl(addr.sin_addr.s_addr), ntohs(addr.sin_port), client_fd};
	CTcpConnection::Pointer pTcpConnection(new CTcpConnection(this, sClient));
	m_SocketInfoManager.Add(sClient, pTcpConnection);

	m_pDataHandle->OnClientConnected(sClient);

}

void CTcpServer::__AsyncCallback(ev::async &watcher, int revents)
{
	FunctionList_t functions;
	
	{
		pthread_spin_lock(&m_Spinlock);

		functions.swap(m_Functions);

		pthread_spin_unlock(&m_Spinlock);
	}  
	
	for (size_t i = 0; i < functions.size(); ++i)  
	{    
		functions[i]();  
	}
}


