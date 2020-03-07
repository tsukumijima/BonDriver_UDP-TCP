// BonDriver.cpp : �C���v�������e�[�V���� �t�@�C��
//

#include "stdafx.h"
#include "BonTuner.h"

/////////////////////////////////////////////////////////////////////////////
// DLLMain�֐�

BOOL APIENTRY DllMain(HINSTANCE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
	switch(ul_reason_for_call){
		case DLL_PROCESS_ATTACH:
			// ���W���[���n���h���ۑ�
			CBonTuner::m_hModule = hModule;
			break;

		case DLL_PROCESS_DETACH:
			// ���J���̏ꍇ�̓C���X�^���X�J��
			if(CBonTuner::m_pThis)CBonTuner::m_pThis->Release();
			break;
		}

	return TRUE;
}
