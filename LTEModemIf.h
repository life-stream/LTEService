#ifndef __LTEMODEMIF_H__
#define __LTEMODEMIF_H__
#include <string>

class CLTEModemIf
{
public:
    static CLTEModemIf& GetInstance();
    virtual ~CLTEModemIf();

    bool InitModem( std::string dev_name );
    int  GetSignalStrengthLevel();
    bool CheckSimCardStatus();
    bool CheckWwanDevStatus( std::string dev_name );
    int  DeregisterFromLTE();
    int  AutomaticRegisterLTE();

private:
    CLTEModemIf();

    //�������̺߳���
    static void GetAtCmdReply( int modem_fd, std::string target, int waiting_ms = 300 );

    int m_fd;   //�����豸�ļ�������

    static std::string ms_reply; //��&������ֵ��
};
#endif
