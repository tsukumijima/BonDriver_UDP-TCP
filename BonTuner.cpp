// BonTuner.cpp: CBonTuner �N���X�̃C���v�������e�[�V����
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "BonTuner.h"

#pragma comment(lib, "ws2_32.lib")

#define UDP_PORT_NUM		10
#define UDP_PORT_NO_START	1234
#define TCP_PORT_NO_START	2230
//#define SUPPORT_IPV6

//////////////////////////////////////////////////////////////////////
// �萔��`
//////////////////////////////////////////////////////////////////////

// �~���[�e�b�N�X��
#ifdef BON_TCP
#define MUTEX_NAME			TEXT("BonDriver_TCP")
#else
#define MUTEX_NAME			TEXT("BonDriver_UDP")
#endif

// ��M�T�C�Y
#define TSDATASIZE			48128			// TS�f�[�^�̃T�C�Y

// FIFO�o�b�t�@�ݒ�
#define ASYNCBUFFTIME		2											// �o�b�t�@�� = 2�b
#define ASYNCBUFFSIZE		( 0x200000 / TSDATASIZE * ASYNCBUFFTIME )	// ����16Mbps�Ƃ���

#define REQRESERVNUM		16				// �񓯊����N�G�X�g�\��
#define REQPOLLINGWAIT		10				// �񓯊����N�G�X�g�|�[�����O�Ԋu(ms)

#define TCP_BLOCKINGWAIT	2000			// �o�b�t�@������Ƃ�TCP��M�𒆒f�ł���Ԋu(ms)


//////////////////////////////////////////////////////////////////////
// �C���X�^���X�������\�b�h
//////////////////////////////////////////////////////////////////////

extern "C" __declspec(dllexport) IBonDriver * CreateBonDriver()
{
	// �X�^���X����(�����̏ꍇ�̓C���X�^���X�̃|�C���^��Ԃ�)
	return (CBonTuner::m_pThis)? CBonTuner::m_pThis : ((IBonDriver *) new CBonTuner);
}


//////////////////////////////////////////////////////////////////////
// �\�z/����
//////////////////////////////////////////////////////////////////////

// �ÓI�����o������
CBonTuner * CBonTuner::m_pThis = NULL;
HINSTANCE CBonTuner::m_hModule = NULL;

CBonTuner::CBonTuner()
	: m_bTunerOpen(false)
	, m_hMutex(NULL)
	, m_pIoReqBuff(NULL)
	, m_pIoPushReq(NULL)
	, m_pIoPopReq(NULL)
	, m_pIoGetReq(NULL)
	, m_dwBusyReqNum(0UL)
	, m_dwReadyReqNum(0UL)
	, m_hPopIoThread(NULL)
	, m_hOnStreamEvent(NULL)
	, m_dwCurSpace(0UL)
	, m_dwCurChannel(0xFFFFFFFFUL)
	, m_sock(INVALID_SOCKET)
{
	m_pThis = this;

	// �N���e�B�J���Z�N�V����������
	::InitializeCriticalSection(&m_CriticalSection);
}

CBonTuner::~CBonTuner()
{
	// �J����Ă�ꍇ�͕���
	CloseTuner();

	// �N���e�B�J���Z�N�V�����폜
	::DeleteCriticalSection(&m_CriticalSection);

	// Winsock�I��
	if (m_bTunerOpen)
		WSACleanup();

	m_pThis = NULL;
}

const BOOL CBonTuner::OpenTuner()
{
	if (!m_bTunerOpen) {
		// Winsock������
		WSADATA stWsa;
		if (WSAStartup(MAKEWORD(2,0),&stWsa) != 0)
			return FALSE;
		m_bTunerOpen = true;
	}

	//return SetChannel(0UL,0UL);

	return TRUE;
}

void CBonTuner::CloseTuner()
{
	// �X���b�h�I���v���Z�b�g
	m_bLoopIoThread = FALSE;

	// �X���b�h�I��
	if(m_hPopIoThread){
		if(::WaitForSingleObject(m_hPopIoThread, 5000) != WAIT_OBJECT_0){
			// �X���b�h�����I��
			::TerminateThread(m_hPopIoThread, 0);
			::OutputDebugString(MUTEX_NAME TEXT(": CBonTuner::CloseTuner() ::TerminateThread(m_hPopIoThread)\n"));
			}

		::CloseHandle(m_hPopIoThread);
		m_hPopIoThread = NULL;
		}

	// �C�x���g�J��
	if(m_hOnStreamEvent){
		::CloseHandle(m_hOnStreamEvent);
		m_hOnStreamEvent = NULL;
		}

	// �o�b�t�@�J��
	FreeIoReqBuff(m_pIoReqBuff);
	m_pIoReqBuff = NULL;
	m_pIoPushReq = NULL;
	m_pIoPopReq = NULL;
	m_pIoGetReq = NULL;
	m_pGettingRxdBuff = NULL;

	m_dwBusyReqNum = 0UL;
	m_dwReadyReqNum = 0UL;

		// �h���C�o�N���[�Y
	if(m_sock != INVALID_SOCKET){
#ifdef BON_TCP
		if(m_clientSock != INVALID_SOCKET){
			closesocket(m_clientSock);
			}
#endif
		closesocket(m_sock);
		m_sock = INVALID_SOCKET;
		}

	// �`�����l��������
	m_dwCurSpace = 0UL;
	m_dwCurChannel = 0xFFFFFFFFUL;

	// �~���[�e�b�N�X�J��
	if(m_hMutex){
		::ReleaseMutex(m_hMutex);
		::CloseHandle(m_hMutex);
		m_hMutex = NULL;
		}
}

const DWORD CBonTuner::WaitTsStream(const DWORD dwTimeOut)
{
	// �I���`�F�b�N
	if(!m_hOnStreamEvent || !m_bLoopIoThread)return WAIT_ABANDONED;

	// �C�x���g���V�O�i����ԂɂȂ�̂�҂�
	const DWORD dwRet = ::WaitForSingleObject(m_hOnStreamEvent, (dwTimeOut)? dwTimeOut : INFINITE);

	switch(dwRet){
		case WAIT_ABANDONED :
			// �`���[�i������ꂽ
			return WAIT_ABANDONED;

		case WAIT_OBJECT_0 :
		case WAIT_TIMEOUT :
			// �X�g���[���擾�\ or �`���[�i������ꂽ
			return (m_bLoopIoThread)? dwRet : WAIT_ABANDONED;

		case WAIT_FAILED :
		default:
			// ��O
			return WAIT_FAILED;
		}
}

const DWORD CBonTuner::GetReadyCount()
{
	// ���o���\TS�f�[�^�����擾����
	return m_dwReadyReqNum;
}

const BOOL CBonTuner::GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	BYTE *pSrc = NULL;

	// TS�f�[�^���o�b�t�@������o��
	if(GetTsStream(&pSrc, pdwSize, pdwRemain)){
		if(*pdwSize){
			::CopyMemory(pDst, pSrc, *pdwSize);
			}

		return TRUE;
		}

	return FALSE;
}

const BOOL CBonTuner::GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	if(!m_pIoGetReq)return FALSE;

	// TS�f�[�^���o�b�t�@������o��
	::EnterCriticalSection(&m_CriticalSection);
	if(m_dwReadyReqNum){
		{

			// �f�[�^�R�s�[
			*pdwSize = m_pIoGetReq->dwRxdSize;
			*ppDst = m_pIoGetReq->RxdBuff;
			m_pGettingRxdBuff = (m_pIoGetReq->dwRxdSize)? m_pIoGetReq->RxdBuff : NULL;

			// �o�b�t�@�ʒu��i�߂�
			m_pIoGetReq = m_pIoGetReq->pNext;
			m_dwReadyReqNum--;
			*pdwRemain = m_dwReadyReqNum;
			::LeaveCriticalSection(&m_CriticalSection);

			return TRUE;
			}
		}
	else{
		// ���o���\�ȃf�[�^���Ȃ�
		*pdwSize = 0;
		*pdwRemain = 0;
		m_pGettingRxdBuff = NULL;
		::LeaveCriticalSection(&m_CriticalSection);

		return TRUE;
		}
}

void CBonTuner::PurgeTsStream()
{
	// �o�b�t�@������o���\�f�[�^���p�[�W����

	::EnterCriticalSection(&m_CriticalSection);
	m_pIoGetReq = m_pIoPopReq;
	m_dwReadyReqNum = 0;
	::LeaveCriticalSection(&m_CriticalSection);
}

void CBonTuner::Release()
{
	// �C���X�^���X�J��
	delete this;
}

LPCTSTR CBonTuner::GetTunerName(void)
{
	// �`���[�i����Ԃ�
#ifdef BON_TCP
	return TEXT("TCP");
#else
	return TEXT("UDP/IPv4");
#endif
}

const BOOL CBonTuner::IsTunerOpening(void)
{
	// �`���[�i�̎g�p���̗L����Ԃ�(�S�v���Z�X��ʂ���)
	HANDLE hMutex = ::OpenMutex(MUTEX_ALL_ACCESS, FALSE, MUTEX_NAME);
	
	if(hMutex){
		// ���Ƀ`���[�i�͊J����Ă���
		::CloseHandle(hMutex);
		return TRUE;
		}
	else{
		// �`���[�i�͊J����Ă��Ȃ�
		return FALSE;
		}
}

LPCTSTR CBonTuner::EnumTuningSpace(const DWORD dwSpace)
{
	// �g�p�\�ȃ`���[�j���O��Ԃ�Ԃ�
	switch(dwSpace){
		case 0UL :	return GetTunerName();
		default  :	return NULL;
		}
}

LPCTSTR CBonTuner::EnumChannelName(const DWORD dwSpace, const DWORD dwChannel)
{
	// �g�p�\�ȃ`�����l����Ԃ�
	if(dwSpace > 0 || (dwChannel >= UDP_PORT_NUM))return NULL;
	static TCHAR buf[32];
#ifdef BON_TCP
	wsprintf(buf,TEXT("Port %d") , TCP_PORT_NO_START + dwChannel);
#else
	wsprintf(buf,TEXT("�|�[�g�ԍ� %d") , UDP_PORT_NO_START + dwChannel);
#endif
	return buf;
}

const DWORD CBonTuner::GetCurSpace(void)
{
	// ���݂̃`���[�j���O��Ԃ�Ԃ�
	return m_dwCurSpace;
}

const DWORD CBonTuner::GetCurChannel(void)
{
	// ���݂̃`�����l����Ԃ�
	return m_dwCurChannel;
}

CBonTuner::AsyncIoReq * CBonTuner::AllocIoReqBuff(const DWORD dwBuffNum)
{
	if(dwBuffNum < 2)return NULL;

	// ���������m�ۂ���
	AsyncIoReq *pNewBuff = new AsyncIoReq [dwBuffNum];
	if(!pNewBuff)return NULL;

	// �[���N���A
	::ZeroMemory(pNewBuff, sizeof(AsyncIoReq) * dwBuffNum);

	// �����N���\�z����
	DWORD dwIndex;
	for(dwIndex = 0 ; dwIndex < ( dwBuffNum - 1 ) ; dwIndex++){
		pNewBuff[dwIndex].pNext= &pNewBuff[dwIndex + 1];
		}

	pNewBuff[dwIndex].pNext = &pNewBuff[0];

	return pNewBuff;
}

void CBonTuner::FreeIoReqBuff(CBonTuner::AsyncIoReq *pBuff)
{
	if(!pBuff)return;

	// �o�b�t�@���J������
	delete [] pBuff;
}

UINT WINAPI CBonTuner::PopIoThread(LPVOID pParam)
{
	CBonTuner *pThis = (CBonTuner *)pParam;

#ifdef BON_TCP
	HANDLE hAcceptEvent = ::CreateEvent(NULL, TRUE, FALSE, NULL);
	::WSAEventSelect(pThis->m_sock, hAcceptEvent, FD_ACCEPT);
#endif

	// �h���C�o��TS�f�[�^���N�G�X�g�𔭍s����
	// �����σ��N�G�X�g���|�[�����O���ă��N�G�X�g������������
	while(pThis->m_bLoopIoThread){

#ifdef BON_TCP
		if(::WaitForSingleObject(hAcceptEvent, 0) == WAIT_OBJECT_0){
			WSANETWORKEVENTS events;
			if(::WSAEnumNetworkEvents(pThis->m_sock, hAcceptEvent, &events) != SOCKET_ERROR && (events.lNetworkEvents & FD_ACCEPT)){
				// TCP�ڑ��v��
				SOCKET clientSock = accept(pThis->m_sock, NULL, NULL);
				if(clientSock != INVALID_SOCKET){
					::WSAEventSelect(clientSock, NULL, 0);
					if(pThis->m_clientSock != INVALID_SOCKET){
						// �ڑ��ς݂Ȃ̂ŋ���
						closesocket(clientSock);
						}
					else{
						// �ڑ�
						pThis->m_clientSock = clientSock;
						pThis->m_dwTcpHeadSize = 0;
						}
					}
				}
			}
		if(pThis->m_clientSock == INVALID_SOCKET){
			::Sleep(REQPOLLINGWAIT);
			continue;
			}
		SOCKET sock = pThis->m_clientSock;
#else
		SOCKET sock = pThis->m_sock;
#endif
		// ���N�G�X�g�����҂����K�薢���Ȃ�ǉ�����
		if(pThis->m_dwBusyReqNum < REQRESERVNUM){

#ifdef BON_TCP
			// �o�b�t�@������Ƃ��͂������҂�
			for(DWORD dwWait = 0; dwWait < TCP_BLOCKINGWAIT && pThis->m_bLoopIoThread; dwWait += REQPOLLINGWAIT){
				::EnterCriticalSection(&pThis->m_CriticalSection);
				if(pThis->m_dwReadyReqNum + pThis->m_dwBusyReqNum < ASYNCBUFFSIZE){
					::LeaveCriticalSection(&pThis->m_CriticalSection);
					break;
					}
				::LeaveCriticalSection(&pThis->m_CriticalSection);
				::Sleep(REQPOLLINGWAIT);
				}
#endif
			::EnterCriticalSection(&pThis->m_CriticalSection);
			if(pThis->m_dwReadyReqNum + pThis->m_dwBusyReqNum >= ASYNCBUFFSIZE){
				// �o�b�t�@����ꂽ
				pThis->PurgeTsStream();
				}
			::LeaveCriticalSection(&pThis->m_CriticalSection);

			// �h���C�o��TS�f�[�^���N�G�X�g�𔭍s����(��M�v���̂�)
			if(!pThis->PushIoRequest(sock)){
				// �G���[����
				sock = INVALID_SOCKET;
				}
			}

		// �����σf�[�^������΃��N�G�X�g����������
		if(sock != INVALID_SOCKET && pThis->m_dwBusyReqNum){

			// ���N�G�X�g����������
			if(!pThis->PopIoRequest(sock)){
				// �G���[����
				sock = INVALID_SOCKET;
				}
			}

		if(sock == INVALID_SOCKET){
#ifdef BON_TCP
			closesocket(pThis->m_clientSock);
			pThis->m_clientSock = INVALID_SOCKET;
			pThis->ClearIoRequest();
#else
			break;
#endif
			}

		if(pThis->m_dwBusyReqNum >= REQRESERVNUM){
			// ���N�G�X�g�����҂����t���̏ꍇ�̓E�F�C�g
			::Sleep(REQPOLLINGWAIT);
			}
		}

#ifdef BON_TCP
	if(pThis->m_clientSock != INVALID_SOCKET){
		closesocket(pThis->m_clientSock);
		}
	::WSAEventSelect(pThis->m_sock, NULL, 0);
	::CloseHandle(hAcceptEvent);
#endif
	closesocket(pThis->m_sock);
	pThis->m_sock = INVALID_SOCKET;
	pThis->ClearIoRequest();

	return 0;
}

void CBonTuner::ClearIoRequest()
{
	// ���ׂĂ̔񓯊����N�G�X�g����擾�Ŋ���������
	while(m_dwBusyReqNum){
		m_pIoPopReq->dwRxdSize = 0;
		if(m_pIoPopReq->OverLapped.hEvent){
			// �C�x���g�폜
			::WaitForSingleObject(m_pIoPopReq->OverLapped.hEvent, INFINITE);
			::CloseHandle(m_pIoPopReq->OverLapped.hEvent);
			m_pIoPopReq->OverLapped.hEvent = NULL;
			}
		// �o�b�t�@��ԍX�V
		::EnterCriticalSection(&m_CriticalSection);
		m_pIoPopReq = m_pIoPopReq->pNext;
		m_dwBusyReqNum--;
		m_dwReadyReqNum++;
		::LeaveCriticalSection(&m_CriticalSection);
		// �C�x���g�Z�b�g
		::SetEvent(m_hOnStreamEvent);
		}
}

const BOOL CBonTuner::PushIoRequest(SOCKET sock)
{
	// �h���C�o�ɔ񓯊����N�G�X�g�𔭍s����

	// �o�b�t�@��ԍX�V
	::EnterCriticalSection(&m_CriticalSection);
	m_pIoPushReq = m_pIoPushReq->pNext;
	m_dwBusyReqNum++;
	if(m_pIoPushReq->RxdBuff == m_pGettingRxdBuff){
		// ���̃o�b�t�@�͎g�p�ł��Ȃ��̂ŋ�擾�Ƃ��Ĕ��s
		::LeaveCriticalSection(&m_CriticalSection);
		return TRUE;
		}
	::LeaveCriticalSection(&m_CriticalSection);

	// �C�x���g�ݒ�
	::ZeroMemory(&m_pIoPushReq->OverLapped, sizeof(WSAOVERLAPPED));
	if(!(m_pIoPushReq->OverLapped.hEvent = ::CreateEvent(NULL, TRUE, FALSE, NULL)))return FALSE;

	// UDP/TCP��M��v���X���j�_�I
	DWORD Flags=0;
	WSABUF wsaBuf;
	wsaBuf.buf = (char*)m_pIoPushReq->RxdBuff;
	wsaBuf.len = sizeof(m_pIoPushReq->RxdBuff);
	if(SOCKET_ERROR==WSARecv(sock,&wsaBuf,1,&m_pIoPushReq->dwRxdSize,&Flags,&m_pIoPushReq->OverLapped,NULL)){
		int sock_err = WSAGetLastError();
		if(sock_err != ERROR_IO_PENDING){
			::CloseHandle(m_pIoPushReq->OverLapped.hEvent);
			m_pIoPushReq->OverLapped.hEvent = NULL;
			return FALSE;
			}
		}

	return TRUE;
}

const BOOL CBonTuner::PopIoRequest(SOCKET sock)
{
	// �񓯊����N�G�X�g����������

	// ���N�G�X�g�擾
	if(m_pIoPopReq->OverLapped.hEvent){
		DWORD Flags=0;
		const BOOL bRet = ::WSAGetOverlappedResult(sock, &m_pIoPopReq->OverLapped, &m_pIoPopReq->dwRxdSize, FALSE,&Flags);

		// �G���[�`�F�b�N
		if(!bRet){
			int sock_err = WSAGetLastError();
			if(sock_err == ERROR_IO_INCOMPLETE){
				// ����������
				return TRUE;
				}
			}

		// �C�x���g�폜
		::CloseHandle(m_pIoPopReq->OverLapped.hEvent);
		m_pIoPopReq->OverLapped.hEvent = NULL;

		if(!bRet){
			// �G���[����
			return FALSE;
			}
#ifdef BON_TCP
		if(m_pIoPopReq->dwRxdSize == 0){
			// �O���[�X�t���ؒf
			return FALSE;
			}
#endif
		}
	else{
		// ��擾
		m_pIoPopReq->dwRxdSize = 0;
		}

#ifdef BON_TCP
	// 8�o�C�g�̓Ǝ��w�b�_��ǂݔ�΂��K�v������
	for(DWORD i = 0, j = 0; j < m_pIoPopReq->dwRxdSize; i++){
		if(m_dwTcpHeadSize < 8){
			m_pIoPopReq->dwRxdSize--;
			// �Ǝ��w�b�_�̌��4�o�C�g�ɓ��e�̃T�C�Y���i�[����Ă���
			if(m_dwTcpHeadSize >= 4){
				((BYTE*)&m_dwTcpContSize)[m_dwTcpHeadSize - 4] = m_pIoPopReq->RxdBuff[i];
				}
			if(++m_dwTcpHeadSize == 8 && m_dwTcpContSize == 0){
				m_dwTcpHeadSize = 0;
				}
			}
		else{
			m_pIoPopReq->RxdBuff[j++] = m_pIoPopReq->RxdBuff[i];
			if(--m_dwTcpContSize == 0){
				m_dwTcpHeadSize = 0;
				}
			}
		}
#endif

	// �o�b�t�@��ԍX�V
	::EnterCriticalSection(&m_CriticalSection);
	m_pIoPopReq = m_pIoPopReq->pNext;
	m_dwBusyReqNum--;
	m_dwReadyReqNum++;
	::LeaveCriticalSection(&m_CriticalSection);

	// �C�x���g�Z�b�g
	::SetEvent(m_hOnStreamEvent);

	return TRUE;
}


// �`�����l���ݒ�
const BOOL CBonTuner::SetChannel(const BYTE bCh)
{
	return SetChannel((DWORD)0,(DWORD)bCh-13);
}

// �`�����l���ݒ�
const BOOL CBonTuner::SetChannel(const DWORD dwSpace, const DWORD dwChannel)
{
	// �L���ȃ`�����l����
	if(dwSpace > 0 || (dwChannel >= UDP_PORT_NUM))
		return FALSE;

	// ��U�N���[�Y
	CloseTuner();

	// �o�b�t�@�m��
	if(!(m_pIoReqBuff = AllocIoReqBuff(ASYNCBUFFSIZE))){
		return FALSE;
		}

	// �o�b�t�@�ʒu����
	m_pIoPushReq = m_pIoReqBuff;
	m_pIoPopReq = m_pIoReqBuff->pNext;
	m_pIoGetReq = m_pIoReqBuff->pNext;
	m_dwBusyReqNum = 0;
	m_dwReadyReqNum = 0;

	try{
		// �h���C�o�I�[�v��
		addrinfo hints = {};
		hints.ai_flags = AI_PASSIVE;
#ifdef SUPPORT_IPV6
		hints.ai_family = AF_INET6;
#else
		hints.ai_family = AF_INET;
#endif
#ifdef BON_TCP
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
		char szPort[16];
		::wsprintfA(szPort, "%d", TCP_PORT_NO_START + dwChannel);
#else
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_UDP;
		char szPort[16];
		::wsprintfA(szPort, "%d", UDP_PORT_NO_START + dwChannel);
#endif
		addrinfo *ai;
		if(getaddrinfo(NULL, szPort, &hints, &ai) != 0){
			throw 1UL;
			}
		m_sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if(m_sock == INVALID_SOCKET){
			freeaddrinfo(ai);
			throw 1UL;
			}
#ifdef BON_TCP
		m_clientSock = INVALID_SOCKET;
		BOOL bReuseAddr = TRUE;
		setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&bReuseAddr, sizeof(bReuseAddr));
#endif
#ifdef SUPPORT_IPV6
		// �f���A���X�^�b�N
		BOOL bV6Only = FALSE;
		setsockopt(m_sock, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&bV6Only, sizeof(bV6Only));
#endif

		// Bind
		if(bind(m_sock, ai->ai_addr, (int)ai->ai_addrlen) == SOCKET_ERROR
#ifdef BON_TCP
			|| listen(m_sock, 1) == SOCKET_ERROR
#endif
			){
			TCHAR szDebugOut[128];
			::wsprintf(szDebugOut, MUTEX_NAME TEXT(": CBonTuner::OpenTuner() bind error %d\n"), WSAGetLastError());
			::OutputDebugString(szDebugOut);
			freeaddrinfo(ai);
			throw 1UL;
		}
		freeaddrinfo(ai);

		// �C�x���g�쐬
		if(!(m_hOnStreamEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL)))throw 2UL;

		// �X���b�h�N��
		m_bLoopIoThread = TRUE;
		if(!(m_hPopIoThread = (HANDLE)_beginthreadex(NULL, 0, PopIoThread, this, 0, NULL))){
			throw 3UL;
			}

		// �~���[�e�b�N�X�쐬
		if(!(m_hMutex = ::CreateMutex(NULL, TRUE, MUTEX_NAME)))throw 5UL;
		}
	catch(const DWORD dwErrorStep){
		// �G���[����
		TCHAR szDebugOut[1024];
		::wsprintf(szDebugOut, MUTEX_NAME TEXT(": CBonTuner::OpenTuner() dwErrorStep = %lu\n"), dwErrorStep);
		::OutputDebugString(szDebugOut);

		CloseTuner();
		return FALSE;
		}

	// �`�����l�����X�V
	m_dwCurSpace = dwSpace;
	m_dwCurChannel = dwChannel;

	// TS�f�[�^�p�[�W
	PurgeTsStream();

	return TRUE;
}

// �M�����x��(�r�b�g���[�g)�擾
const float CBonTuner::GetSignalLevel(void)
{
	// �M�����x���͏��0
	return 0.0f;
}
