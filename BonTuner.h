// BonTuner.h: CBonTuner �N���X�̃C���^�[�t�F�C�X
//
//////////////////////////////////////////////////////////////////////

#if !defined(_BONTUNER_H_)
#define _BONTUNER_H_

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000


#define dllimport dllexport
#include "IBonDriver2.h"


class CBonTuner : public IBonDriver2
{
public:
	CBonTuner();
	virtual ~CBonTuner();

// IBonDriver
	const BOOL OpenTuner(void);
	void CloseTuner(void);

	const BOOL SetChannel(const BYTE bCh);
	const float GetSignalLevel(void);

	const DWORD WaitTsStream(const DWORD dwTimeOut = 0);
	const DWORD GetReadyCount(void);

	const BOOL GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain);
	const BOOL GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain);

	void PurgeTsStream(void);

// IBonDriver2(�b��)
	LPCTSTR GetTunerName(void);

	const BOOL IsTunerOpening(void);

	LPCTSTR EnumTuningSpace(const DWORD dwSpace);
	LPCTSTR EnumChannelName(const DWORD dwSpace, const DWORD dwChannel);

	const BOOL SetChannel(const DWORD dwSpace, const DWORD dwChannel);

	const DWORD GetCurSpace(void);
	const DWORD GetCurChannel(void);

	void Release(void);

	static CBonTuner * m_pThis;
	static HINSTANCE m_hModule;

protected:
	// I/O���N�G�X�g�L���[�f�[�^
	struct AsyncIoReq
	{
		WSAOVERLAPPED OverLapped;
		DWORD dwRxdSize;
		BYTE RxdBuff[48128];
		AsyncIoReq *pNext;
	};

	AsyncIoReq * AllocIoReqBuff(const DWORD dwBuffNum);
	void FreeIoReqBuff(AsyncIoReq *pBuff);

	static UINT WINAPI PopIoThread(LPVOID pParam);

	void ClearIoRequest();
	const BOOL PushIoRequest(SOCKET sock);
	const BOOL PopIoRequest(SOCKET sock);

	bool m_bTunerOpen;

	HANDLE m_hMutex;

	AsyncIoReq *m_pIoReqBuff;
	AsyncIoReq *m_pIoPushReq;
	AsyncIoReq *m_pIoPopReq;
	AsyncIoReq *m_pIoGetReq;
	BYTE *m_pGettingRxdBuff;

	DWORD m_dwBusyReqNum;
	DWORD m_dwReadyReqNum;

	HANDLE m_hPopIoThread;
	BOOL m_bLoopIoThread;

	HANDLE m_hOnStreamEvent;

	CRITICAL_SECTION m_CriticalSection;

	DWORD m_dwCurSpace;
	DWORD m_dwCurChannel;

	// �ǉ� byMeru(2008/03/27)
	SOCKET m_sock;

#ifdef BON_TCP
	SOCKET m_clientSock;
	DWORD m_dwTcpHeadSize;
	DWORD m_dwTcpContSize;
#endif
};

#endif // !defined(_BONTUNER_H_)
