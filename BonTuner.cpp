// BonTuner.cpp: CBonTuner クラスのインプリメンテーション
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
// 定数定義
//////////////////////////////////////////////////////////////////////

// ミューテックス名
#ifdef BON_TCP
#define MUTEX_NAME			TEXT("BonDriver_TCP")
#else
#define MUTEX_NAME			TEXT("BonDriver_UDP")
#endif

// 受信サイズ
#define TSDATASIZE			48128			// TSデータのサイズ

// FIFOバッファ設定
#define ASYNCBUFFTIME		2											// バッファ長 = 2秒
#define ASYNCBUFFSIZE		( 0x200000 / TSDATASIZE * ASYNCBUFFTIME )	// 平均16Mbpsとする

#define REQRESERVNUM		16				// 非同期リクエスト予約数
#define REQPOLLINGWAIT		10				// 非同期リクエストポーリング間隔(ms)

#define TCP_BLOCKINGWAIT	2000			// バッファが溢れるときTCP受信を中断できる間隔(ms)


//////////////////////////////////////////////////////////////////////
// インスタンス生成メソッド
//////////////////////////////////////////////////////////////////////

extern "C" __declspec(dllexport) IBonDriver * CreateBonDriver()
{
	// スタンス生成(既存の場合はインスタンスのポインタを返す)
	return (CBonTuner::m_pThis)? CBonTuner::m_pThis : ((IBonDriver *) new CBonTuner);
}


//////////////////////////////////////////////////////////////////////
// 構築/消滅
//////////////////////////////////////////////////////////////////////

// 静的メンバ初期化
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

	// クリティカルセクション初期化
	::InitializeCriticalSection(&m_CriticalSection);
}

CBonTuner::~CBonTuner()
{
	// 開かれてる場合は閉じる
	CloseTuner();

	// クリティカルセクション削除
	::DeleteCriticalSection(&m_CriticalSection);

	// Winsock終了
	if (m_bTunerOpen)
		WSACleanup();

	m_pThis = NULL;
}

const BOOL CBonTuner::OpenTuner()
{
	if (!m_bTunerOpen) {
		// Winsock初期化
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
	// スレッド終了要求セット
	m_bLoopIoThread = FALSE;

	// スレッド終了
	if(m_hPopIoThread){
		if(::WaitForSingleObject(m_hPopIoThread, 5000) != WAIT_OBJECT_0){
			// スレッド強制終了
			::TerminateThread(m_hPopIoThread, 0);
			::OutputDebugString(MUTEX_NAME TEXT(": CBonTuner::CloseTuner() ::TerminateThread(m_hPopIoThread)\n"));
			}

		::CloseHandle(m_hPopIoThread);
		m_hPopIoThread = NULL;
		}

	// イベント開放
	if(m_hOnStreamEvent){
		::CloseHandle(m_hOnStreamEvent);
		m_hOnStreamEvent = NULL;
		}

	// バッファ開放
	FreeIoReqBuff(m_pIoReqBuff);
	m_pIoReqBuff = NULL;
	m_pIoPushReq = NULL;
	m_pIoPopReq = NULL;
	m_pIoGetReq = NULL;
	m_pGettingRxdBuff = NULL;

	m_dwBusyReqNum = 0UL;
	m_dwReadyReqNum = 0UL;

		// ドライバクローズ
	if(m_sock != INVALID_SOCKET){
#ifdef BON_TCP
		if(m_clientSock != INVALID_SOCKET){
			closesocket(m_clientSock);
			}
#endif
		closesocket(m_sock);
		m_sock = INVALID_SOCKET;
		}

	// チャンネル初期化
	m_dwCurSpace = 0UL;
	m_dwCurChannel = 0xFFFFFFFFUL;

	// ミューテックス開放
	if(m_hMutex){
		::ReleaseMutex(m_hMutex);
		::CloseHandle(m_hMutex);
		m_hMutex = NULL;
		}
}

const DWORD CBonTuner::WaitTsStream(const DWORD dwTimeOut)
{
	// 終了チェック
	if(!m_hOnStreamEvent || !m_bLoopIoThread)return WAIT_ABANDONED;

	// イベントがシグナル状態になるのを待つ
	const DWORD dwRet = ::WaitForSingleObject(m_hOnStreamEvent, (dwTimeOut)? dwTimeOut : INFINITE);

	switch(dwRet){
		case WAIT_ABANDONED :
			// チューナが閉じられた
			return WAIT_ABANDONED;

		case WAIT_OBJECT_0 :
		case WAIT_TIMEOUT :
			// ストリーム取得可能 or チューナが閉じられた
			return (m_bLoopIoThread)? dwRet : WAIT_ABANDONED;

		case WAIT_FAILED :
		default:
			// 例外
			return WAIT_FAILED;
		}
}

const DWORD CBonTuner::GetReadyCount()
{
	// 取り出し可能TSデータ数を取得する
	return m_dwReadyReqNum;
}

const BOOL CBonTuner::GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	BYTE *pSrc = NULL;

	// TSデータをバッファから取り出す
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

	// TSデータをバッファから取り出す
	::EnterCriticalSection(&m_CriticalSection);
	if(m_dwReadyReqNum){
		{

			// データコピー
			*pdwSize = m_pIoGetReq->dwRxdSize;
			*ppDst = m_pIoGetReq->RxdBuff;
			m_pGettingRxdBuff = (m_pIoGetReq->dwRxdSize)? m_pIoGetReq->RxdBuff : NULL;

			// バッファ位置を進める
			m_pIoGetReq = m_pIoGetReq->pNext;
			m_dwReadyReqNum--;
			*pdwRemain = m_dwReadyReqNum;
			::LeaveCriticalSection(&m_CriticalSection);

			return TRUE;
			}
		}
	else{
		// 取り出し可能なデータがない
		*pdwSize = 0;
		*pdwRemain = 0;
		m_pGettingRxdBuff = NULL;
		::LeaveCriticalSection(&m_CriticalSection);

		return TRUE;
		}
}

void CBonTuner::PurgeTsStream()
{
	// バッファから取り出し可能データをパージする

	::EnterCriticalSection(&m_CriticalSection);
	m_pIoGetReq = m_pIoPopReq;
	m_dwReadyReqNum = 0;
	::LeaveCriticalSection(&m_CriticalSection);
}

void CBonTuner::Release()
{
	// インスタンス開放
	delete this;
}

LPCTSTR CBonTuner::GetTunerName(void)
{
	// チューナ名を返す
#ifdef BON_TCP
	return TEXT("TCP");
#else
	return TEXT("UDP/IPv4");
#endif
}

const BOOL CBonTuner::IsTunerOpening(void)
{
	// チューナの使用中の有無を返す(全プロセスを通して)
	HANDLE hMutex = ::OpenMutex(MUTEX_ALL_ACCESS, FALSE, MUTEX_NAME);
	
	if(hMutex){
		// 既にチューナは開かれている
		::CloseHandle(hMutex);
		return TRUE;
		}
	else{
		// チューナは開かれていない
		return FALSE;
		}
}

LPCTSTR CBonTuner::EnumTuningSpace(const DWORD dwSpace)
{
	// 使用可能なチューニング空間を返す
	switch(dwSpace){
		case 0UL :	return GetTunerName();
		default  :	return NULL;
		}
}

LPCTSTR CBonTuner::EnumChannelName(const DWORD dwSpace, const DWORD dwChannel)
{
	// 使用可能なチャンネルを返す
	if(dwSpace > 0 || (dwChannel >= UDP_PORT_NUM))return NULL;
	static TCHAR buf[32];
#ifdef BON_TCP
	wsprintf(buf,TEXT("Port %d") , TCP_PORT_NO_START + dwChannel);
#else
	wsprintf(buf,TEXT("ポート番号 %d") , UDP_PORT_NO_START + dwChannel);
#endif
	return buf;
}

const DWORD CBonTuner::GetCurSpace(void)
{
	// 現在のチューニング空間を返す
	return m_dwCurSpace;
}

const DWORD CBonTuner::GetCurChannel(void)
{
	// 現在のチャンネルを返す
	return m_dwCurChannel;
}

CBonTuner::AsyncIoReq * CBonTuner::AllocIoReqBuff(const DWORD dwBuffNum)
{
	if(dwBuffNum < 2)return NULL;

	// メモリを確保する
	AsyncIoReq *pNewBuff = new AsyncIoReq [dwBuffNum];
	if(!pNewBuff)return NULL;

	// ゼロクリア
	::ZeroMemory(pNewBuff, sizeof(AsyncIoReq) * dwBuffNum);

	// リンクを構築する
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

	// バッファを開放する
	delete [] pBuff;
}

UINT WINAPI CBonTuner::PopIoThread(LPVOID pParam)
{
	CBonTuner *pThis = (CBonTuner *)pParam;

#ifdef BON_TCP
	HANDLE hAcceptEvent = ::CreateEvent(NULL, TRUE, FALSE, NULL);
	::WSAEventSelect(pThis->m_sock, hAcceptEvent, FD_ACCEPT);
#endif

	// ドライバにTSデータリクエストを発行する
	// 処理済リクエストをポーリングしてリクエストを完了させる
	while(pThis->m_bLoopIoThread){

#ifdef BON_TCP
		if(::WaitForSingleObject(hAcceptEvent, 0) == WAIT_OBJECT_0){
			WSANETWORKEVENTS events;
			if(::WSAEnumNetworkEvents(pThis->m_sock, hAcceptEvent, &events) != SOCKET_ERROR && (events.lNetworkEvents & FD_ACCEPT)){
				// TCP接続要求
				SOCKET clientSock = accept(pThis->m_sock, NULL, NULL);
				if(clientSock != INVALID_SOCKET){
					::WSAEventSelect(clientSock, NULL, 0);
					if(pThis->m_clientSock != INVALID_SOCKET){
						// 接続済みなので拒否
						closesocket(clientSock);
						}
					else{
						// 接続
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
		// リクエスト処理待ちが規定未満なら追加する
		if(pThis->m_dwBusyReqNum < REQRESERVNUM){

#ifdef BON_TCP
			// バッファが溢れるときはすこし待つ
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
				// バッファが溢れた
				pThis->PurgeTsStream();
				}
			::LeaveCriticalSection(&pThis->m_CriticalSection);

			// ドライバにTSデータリクエストを発行する(受信要求のみ)
			if(!pThis->PushIoRequest(sock)){
				// エラー発生
				sock = INVALID_SOCKET;
				}
			}

		// 処理済データがあればリクエストを完了する
		if(sock != INVALID_SOCKET && pThis->m_dwBusyReqNum){

			// リクエストを完了する
			if(!pThis->PopIoRequest(sock)){
				// エラー発生
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
			// リクエスト処理待ちがフルの場合はウェイト
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
	// すべての非同期リクエストを空取得で完了させる
	while(m_dwBusyReqNum){
		m_pIoPopReq->dwRxdSize = 0;
		if(m_pIoPopReq->OverLapped.hEvent){
			// イベント削除
			::WaitForSingleObject(m_pIoPopReq->OverLapped.hEvent, INFINITE);
			::CloseHandle(m_pIoPopReq->OverLapped.hEvent);
			m_pIoPopReq->OverLapped.hEvent = NULL;
			}
		// バッファ状態更新
		::EnterCriticalSection(&m_CriticalSection);
		m_pIoPopReq = m_pIoPopReq->pNext;
		m_dwBusyReqNum--;
		m_dwReadyReqNum++;
		::LeaveCriticalSection(&m_CriticalSection);
		// イベントセット
		::SetEvent(m_hOnStreamEvent);
		}
}

const BOOL CBonTuner::PushIoRequest(SOCKET sock)
{
	// ドライバに非同期リクエストを発行する

	// バッファ状態更新
	::EnterCriticalSection(&m_CriticalSection);
	m_pIoPushReq = m_pIoPushReq->pNext;
	m_dwBusyReqNum++;
	if(m_pIoPushReq->RxdBuff == m_pGettingRxdBuff){
		// このバッファは使用できないので空取得として発行
		::LeaveCriticalSection(&m_CriticalSection);
		return TRUE;
		}
	::LeaveCriticalSection(&m_CriticalSection);

	// イベント設定
	::ZeroMemory(&m_pIoPushReq->OverLapped, sizeof(WSAOVERLAPPED));
	if(!(m_pIoPushReq->OverLapped.hEvent = ::CreateEvent(NULL, TRUE, FALSE, NULL)))return FALSE;

	// UDP/TCP受信を要求スルニダ！
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
	// 非同期リクエストを完了する

	// リクエスト取得
	if(m_pIoPopReq->OverLapped.hEvent){
		DWORD Flags=0;
		const BOOL bRet = ::WSAGetOverlappedResult(sock, &m_pIoPopReq->OverLapped, &m_pIoPopReq->dwRxdSize, FALSE,&Flags);

		// エラーチェック
		if(!bRet){
			int sock_err = WSAGetLastError();
			if(sock_err == ERROR_IO_INCOMPLETE){
				// 処理未完了
				return TRUE;
				}
			}

		// イベント削除
		::CloseHandle(m_pIoPopReq->OverLapped.hEvent);
		m_pIoPopReq->OverLapped.hEvent = NULL;

		if(!bRet){
			// エラー発生
			return FALSE;
			}
#ifdef BON_TCP
		if(m_pIoPopReq->dwRxdSize == 0){
			// グレースフル切断
			return FALSE;
			}
#endif
		}
	else{
		// 空取得
		m_pIoPopReq->dwRxdSize = 0;
		}

#ifdef BON_TCP
	// 8バイトの独自ヘッダを読み飛ばす必要がある
	for(DWORD i = 0, j = 0; j < m_pIoPopReq->dwRxdSize; i++){
		if(m_dwTcpHeadSize < 8){
			m_pIoPopReq->dwRxdSize--;
			// 独自ヘッダの後ろ4バイトに内容のサイズが格納されている
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

	// バッファ状態更新
	::EnterCriticalSection(&m_CriticalSection);
	m_pIoPopReq = m_pIoPopReq->pNext;
	m_dwBusyReqNum--;
	m_dwReadyReqNum++;
	::LeaveCriticalSection(&m_CriticalSection);

	// イベントセット
	::SetEvent(m_hOnStreamEvent);

	return TRUE;
}


// チャンネル設定
const BOOL CBonTuner::SetChannel(const BYTE bCh)
{
	return SetChannel((DWORD)0,(DWORD)bCh-13);
}

// チャンネル設定
const BOOL CBonTuner::SetChannel(const DWORD dwSpace, const DWORD dwChannel)
{
	// 有効なチャンネルか
	if(dwSpace > 0 || (dwChannel >= UDP_PORT_NUM))
		return FALSE;

	// 一旦クローズ
	CloseTuner();

	// バッファ確保
	if(!(m_pIoReqBuff = AllocIoReqBuff(ASYNCBUFFSIZE))){
		return FALSE;
		}

	// バッファ位置同期
	m_pIoPushReq = m_pIoReqBuff;
	m_pIoPopReq = m_pIoReqBuff->pNext;
	m_pIoGetReq = m_pIoReqBuff->pNext;
	m_dwBusyReqNum = 0;
	m_dwReadyReqNum = 0;

	try{
		// ドライバオープン
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
		// デュアルスタック
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

		// イベント作成
		if(!(m_hOnStreamEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL)))throw 2UL;

		// スレッド起動
		m_bLoopIoThread = TRUE;
		if(!(m_hPopIoThread = (HANDLE)_beginthreadex(NULL, 0, PopIoThread, this, 0, NULL))){
			throw 3UL;
			}

		// ミューテックス作成
		if(!(m_hMutex = ::CreateMutex(NULL, TRUE, MUTEX_NAME)))throw 5UL;
		}
	catch(const DWORD dwErrorStep){
		// エラー発生
		TCHAR szDebugOut[1024];
		::wsprintf(szDebugOut, MUTEX_NAME TEXT(": CBonTuner::OpenTuner() dwErrorStep = %lu\n"), dwErrorStep);
		::OutputDebugString(szDebugOut);

		CloseTuner();
		return FALSE;
		}

	// チャンネル情報更新
	m_dwCurSpace = dwSpace;
	m_dwCurChannel = dwChannel;

	// TSデータパージ
	PurgeTsStream();

	return TRUE;
}

// 信号レベル(ビットレート)取得
const float CBonTuner::GetSignalLevel(void)
{
	// 信号レベルは常に0
	return 0.0f;
}
