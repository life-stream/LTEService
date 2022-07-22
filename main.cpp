#include "LTEModemIf.h"
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <uci.h>
#include <sstream>
#include <string>
#include "Log.h"
#include <cstdlib>
#include <resolv.h> //res_init
#include <errno.h>
#include <cstring> //strerror

using namespace std;

typedef struct _ConnArgs
{
    string connect_exe;
    string wwan_dev;
    string tty_dev;
    string ping_target;
    int    ping_interval;
} ConnArgs;

typedef struct _LogArgs
{
    string file_name;
    int    lv;
} LogArgs;

typedef struct _ICMPPack
{
    unsigned char  type;
    unsigned char  code;
    unsigned short checksum;
    unsigned short identifier;
    unsigned short seqnum;
    unsigned long  time;
} ICMPPack;

enum EN_MODEM_ACTION{
    EN_NONE = 0,
    EN_RESET,
    EN_DEREGISTER,
    EN_REGISTER,
    EN_CONNECT
};

static pid_t s_cm_pid{-1};

unsigned short Checksum( void* buffer, int len )
{
    LogOutLine( "Checksum called.", 3 );

    unsigned short* p {( unsigned short* )buffer};
    int sum {0};

    while( len > 1 )
    {
        sum += *p++;
        len -= 2;
    }
    if( len == 1 )
    {
        unsigned short t {0};
        *( unsigned char* )( &t ) = *( unsigned char* )p;
        sum += t;
    }

    sum = ( sum >> 16 ) + ( sum & 0xffff );
    sum += ( sum >> 16 );
    return( ~sum );
}

int Ping( const char* target_str, unsigned short num = 1 )
{
    LogOutLine( "Ping start. num: " + to_string( num ), 3 );
    LogOutChars( target_str, 3 );

    //�ж����������������IP��ַ
    unsigned int addr = inet_addr( target_str );
    if( addr == INADDR_NONE )   //������
    {
        hostent* p_hostent = gethostbyname( target_str );
        if( !p_hostent || !p_hostent->h_addr )
        {
            LogOutLine( "Get host failed.", 1 );
            return -1;  //һ����DNSû����
        }

        memcpy( &addr, p_hostent->h_addr_list[0], p_hostent->h_length );
    }

    struct sockaddr_in dest_addr;
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_addr.s_addr = addr;

    //socket
    int sock_fd = socket( AF_INET, SOCK_RAW, IPPROTO_ICMP ); //ע����rootȨ�ޡ�
    if( sock_fd < 0 )
    {
        LogOutLine( "Get socket failed.", 1 );
        return -2;  //�˳�ϵͳ������ϡ�
    }

    int res {0};

    for( int i {0}; i < num; i++ )
    { 
        //ICMP�������ʼ��
        ICMPPack request;
        request.type       = 8;           //�ɴ����ж�
        request.code       = 0;           //Ҫ��ر�
        request.checksum   = 0;
        request.identifier = getpid();    //�ضϿ��ܣ��ް�
        request.seqnum     = i;           //���

        struct timeval time;
        gettimeofday( &time, NULL );
        request.time = time.tv_usec;      //����Ϊ����ʱ�̣�΢��

        request.checksum = Checksum( &request, sizeof( request ) );

        if( -1 == sendto( sock_fd, &request, sizeof( request ), 0, ( struct sockaddr* )&dest_addr, sizeof( dest_addr ) ) )
        {
            res += 5; //����ʱ�豸û��?
            LogOutLine( "Ping: sendto failed. res + 5", 3 );
            continue;
        }

        unsigned char recvbuf[1500];
        struct sockaddr_in src_addr;
        socklen_t size = sizeof( sockaddr_in ); //����ʼ��ȡ�������͵�ֵַ������Ǿݴ˷���sockaddr�Ŀռ䡣

        if( -1 == recvfrom( sock_fd, recvbuf, sizeof( recvbuf ), 0, ( struct sockaddr* )&src_addr, &size ) )
        {
            res += 4; //����ʱû��?
            LogOutLine( "Ping: recvfrom failed. res + 4", 3 );
            continue;
        }

        gettimeofday( &time, NULL );    //�ر�����ʱ��

        unsigned char h { ( recvbuf[0] & 0x0f ) * 4 }; //IP��ͷ���ȼ���

        ICMPPack reply;
        memcpy( &reply, recvbuf + h, sizeof( reply ) );

        //request.identifier������ֱ��getpid
        if( ( reply.identifier != request.identifier ) ||
            ( reply.type != 0 ) ||
            ( src_addr.sin_addr.s_addr != dest_addr.sin_addr.s_addr ) ||
            ( reply.seqnum != i ) )
        {
            res += 2; //�ذ�����
            LogOutLine( "Ping: reply error. res + 2", 3 );
            continue;
        }

        //����������ʱ����ӳ�1�룬����ʱ��ҳ���ǿ��ԴպϿ����ٶ���Ƕ�������û�����ˡ�
        unsigned long cost { ( time.tv_usec - reply.time ) / 1000 };  //ms
        if( cost >  1500 )
        {
            LogOutLine( "Ping: reply slowly. res + 1", 3 );
            res += 1;   //ͨ���ٶȹ���
        }
    }

    close( sock_fd );
    return res;
}

void QuitMnets( int sig )
{
    if( s_cm_pid != -1 )
    {
        kill( s_cm_pid, 15 );
        s_cm_pid = -1;

        LogOutLine( "Connect exe quit." );
    }

    DirectOutLine( "Mnets quit." );
    CloseLogFile();
    exit( 0 );
}

bool LoadConfigFile( string& file_name, ConnArgs& conn )
{
    LogOutLine( "LoadConfigFile "+ file_name +" start." );

    struct uci_context* p_context = uci_alloc_context();
    struct uci_package* p_pkg = NULL;

    if( uci_load( p_context, file_name.c_str(), &p_pkg ) != UCI_OK )
    {
        LogOutLine( "uci_load failed." );
        uci_free_context( p_context );
        return false;
    }

    struct uci_section* p_section = uci_lookup_section( p_context, p_pkg, "conn" );
    if( !p_section )
    {
        LogOutLine( "uci_lookup_section error." );
        uci_unload( p_context, p_pkg );
        uci_free_context( p_context );
        return false;
    }
    LogOutLine( "ConnectArg: conn", 2 );

    stringstream ss;
    const char* p_ping_target = uci_lookup_option_string( p_context, p_section, "ping_target" );
    if( !p_ping_target )
    {
        LogOutLine( "Lookup option ping_target error.", 1 );
        uci_unload( p_context, p_pkg );
        uci_free_context( p_context );
        return false;
    }
    else
    {
        ss << p_ping_target;
        getline( ss, conn.ping_target );
    }
    LogOutLine( "ping_target: " + conn.ping_target, 2 );

    const char* p_connect_exe = uci_lookup_option_string( p_context, p_section, "connect_exe" );
    if( !p_connect_exe )
    {
        LogOutLine( "Lookup option connect_exe error.", 1 );
        uci_unload( p_context, p_pkg );
        uci_free_context( p_context );
        return false;
    }
    else
    {
        ss.str( "" );ss.clear();
        ss << p_connect_exe;
        getline( ss, conn.connect_exe );
    }
    LogOutLine( "connect_exe: " + conn.connect_exe, 2 );

    const char* p_wwan_dev = uci_lookup_option_string( p_context, p_section, "wwan_dev" );
    if( !p_wwan_dev )
    {
        LogOutLine( "Lookup option wwan_dev error.", 1 );
        uci_unload( p_context, p_pkg );
        uci_free_context( p_context );
        return false;
    }
    else
    {
        ss.str( "" );ss.clear();
        ss << p_wwan_dev;
        getline( ss, conn.wwan_dev );
    }
    LogOutLine( "wwan_dev: " + conn.wwan_dev, 2 );

    const char* p_tty_dev = uci_lookup_option_string( p_context, p_section, "tty_dev" );
    if( !p_tty_dev )
    {
        LogOutLine( "Lookup option tty_dev error.", 1 );
        uci_unload( p_context, p_pkg );
        uci_free_context( p_context );
        return false;
    }
    else
    {
        ss.str( "" );ss.clear();
        ss << p_tty_dev;
        getline( ss, conn.tty_dev );
    }
    LogOutLine( "tty_dev: " + conn.tty_dev, 2 );

    const char* p_ping_interval = uci_lookup_option_string( p_context, p_section, "ping_interval" );
    if( !p_ping_interval )
    {
        conn.ping_interval = 30;
    }
    else
    {
        conn.ping_interval = atoi( p_ping_interval );
    }
    LogOutLine( "ping_interval: " + to_string( conn.ping_interval ), 2 );

    uci_unload( p_context, p_pkg );
    uci_free_context( p_context );
    return true;
}

int main( int argc, char* argv[] )
{
    ConnArgs conn;
    LogArgs log;
    int c;

    while( ( c = getopt( argc, argv, "r:n:" ) ) != -1 )
    {
        switch( c )
        {
            case 'r':
                log.file_name = optarg;
                break;
            case 'n':
                log.lv = optarg ? stoi( optarg ) : 0;
                break;
        }
    }

    //log�ļ���log�ȼ�
    //����logʱ�ȼ�Ϊ�����������ļ�
    if( !log.file_name.empty() )
    {
        if ( !SetLogFile( log.file_name ) ) return 1;
        SetOutLevel( log.lv );
    }
    else
    {
        SetOutLevel( -1 );
    }


    //�������ļ�
    string conf_file_name = ( optind < argc ) ? argv[optind] : "/etc/config/mnets";

    if( !LoadConfigFile( conf_file_name, conn ) )
    {
        DirectOutLine( "Load config file failed." );
        CloseLogFile();
        return 0;
    }

    signal( SIGCHLD, SIG_IGN );
    signal( SIGTERM, QuitMnets );
    signal( SIGINT, QuitMnets );
    signal( SIGQUIT, QuitMnets );

    LogOutLine( "Mnets start." );

    int ping_interval = conn.ping_interval;  //pingʱ����

    while( true )
    {
        //������ԡ�
        int ping_res {-1};
        int ping_count {4};
        ping_res = Ping( conn.ping_target.c_str(), ping_count );
        LogOutLine( "ping_res: " + to_string( ping_res ), 1 );

        //״̬���þ�ֱ���˳�
        if( ( ping_res < 4 ) && ( ping_res >= 0 ) )
        {
            sleep( ping_interval );
            continue;
        }

        //����
        CLTEModemIf& modem = CLTEModemIf::GetInstance();
        if( !modem.InitModem( conn.tty_dev ) )
        {
            LogOutLine( "Modem offline." );
            break;
        }

        //���ٲ����������������ӻ�վ������
        if( ( ping_res >= 4 ) && ( ping_res <= 7 ) )
        {
            LogOutLine( "Too slow. Test reconnected." );

            EN_MODEM_ACTION next_sp{EN_DEREGISTER},last_sp{EN_NONE};

            while( next_sp != EN_NONE )
            {
                switch( next_sp )
                {
                    case EN_RESET:
                    {
                        last_sp = EN_RESET;

                        if( modem.ResetUserEquipment() == 0 )
                        {
                            sleep( 1 );
                            next_sp = EN_CONNECT;
                        }
                        else
                        {
                            LogOutLine( "Reset failed." );
                            LogOutLine( "Check the connection status of device.");
                            QuitMnets( 15 );
                        }
                        break;
                    }
                    case EN_DEREGISTER:
                    {
                        last_sp = EN_DEREGISTER;
                        kill( s_cm_pid, 15 );
                        s_cm_pid = -1;

                        if( ( modem.DeregisterFromMNet() == 0 ) )
                        {
                            sleep( 1 );
                            next_sp = EN_REGISTER;
                        }
                        else
                        {
                            LogOutLine( "Deregister failed." );
                            next_sp = EN_RESET;
                        }
                        break;
                    }
                    case EN_REGISTER:
                    {
                        last_sp = EN_REGISTER;

                        if( modem.AutoRegisterMNet() == 0 )
                        {
                            next_sp = EN_CONNECT;
                        }
                        else
                        {
                            LogOutLine( "Register failed." );
                            next_sp = EN_RESET;
                        }
                        break;
                    }
                    case EN_CONNECT:
                    {
                        last_sp = EN_CONNECT;

                        if( ( s_cm_pid = fork() ) < 0 )
                        {
                            LogOutLine( "Connect exe failed." );
                            //��ô����
                        }
                        if( s_cm_pid == 0 )
                        {
                            LogOutLine( "Connect exe start." );
                            int s = conn.connect_exe.find_last_of( '/' ) + 1;
                            LogOutChars( &conn.connect_exe[s], 3 );
                            execl( conn.connect_exe.c_str(), conn.connect_exe.c_str() + s, NULL ); //--------todo: args?
                        }
                        sleep( 15 );
                        res_init();

                        ping_res = Ping( conn.ping_target.c_str(), ping_count );

                        if( ( last_sp == EN_REGISTER ) && ( ping_res >= 4 ) )
                        {
                            kill( s_cm_pid, 15 );
                            s_cm_pid = -1;
                            next_sp = EN_RESET;
                            break;
                        }

                        //�������Ļ�������Ҳ��������
                        if( ( ping_res <= 7 ) && ( ping_res >= 0 ) )
                        {
                            sleep( ping_interval );
                            next_sp = EN_NONE;
                        }
                        break;
                    }
                    default:
                    {
                        LogOutLine( "Unknown action enum value: " + to_string( next_sp ) );
                        break;
                    }
                }
            }//while( next_sp != EN_NONE ) end
        } //if( ( ping_res >= 4 ) && ( ping_res <= 7 ) ) end

        //������Ϊ���Ѿ�û�ˡ���¼��־�����״̬�����²���
        LogOutLine( "Info: current errno " + to_string( errno ) + " " + strerror( errno ), 3 );

        if( s_cm_pid != -1 )
        {
            kill( s_cm_pid, 15 );
            s_cm_pid = -1;
            LogOutLine( "Last connect exe quit." );
        }

        //socket��ȡʧ����
        if( ping_res == -2 )
        {
            LogOutLine( "Create socket failed." );
            break;
        }

        //wwan�豸���߷�
        if( !modem.CheckWwanDevStatus( conn.wwan_dev ) )
        {
            LogOutLine( "Wwan device lost." );
            break;
        }

        //sim��״̬���
        bool sim_aru{false};
        for(int i {0}; i < 3; i++ )
        {
            sim_aru |= modem.CheckSimCardStatus();
            if( sim_aru ) break;
            else sleep( 2 );
        }
        if( !sim_aru )
        {
            LogOutLine( "SimCard lost." );
            break;
        }

        //�ź�ǿ�ȼ��
        int i {0};
        for( ; i < 30; i++ )
        {
            if( modem.GetSignalStrengthLevel() > 0 )
            {
                break;
            }
            else
            {
                LogOutLine( "Signal weak." );
                sleep( 10 );
            }
        }

        //һֱ���?
        while( i == 30 )
        {
            if( ( modem.DeregisterFromMNet() != 0 ) || ( modem.AutoRegisterMNet() != 0 ) )
            {
                LogOutLine( "Net register failed.", 1 );
            }
            else
            {
                if( modem.GetSignalStrengthLevel() > 0 )
                {
                    break;
                }
            }

            if( modem.ResetUserEquipment() != 0 )
            {
                LogOutLine( "ResetUserEquipment failed.", 1 );
                sleep( 60 );

                if( modem.ResetUserEquipment() != 0 )
                {
                    LogOutLine( "Reset failed twice. Check the device.");
                    QuitMnets( 15 );
                }
            }
            if( modem.GetSignalStrengthLevel() > 0 )
            {
                break;
            }
            else
            {
                LogOutLine( "Signal weak continue." );
                sleep( 120 );
            }
        }

        //����
        if( ( s_cm_pid = fork() ) < 0 )
        {
            LogOutLine( "Connect exe failed." );
            break;
        }
        if( s_cm_pid == 0 )
        {
            LogOutLine( "Connect exe start." );
            int s = conn.connect_exe.find_last_of( '/' ) + 1;
            LogOutChars( &conn.connect_exe[s], 3 );
            execl( conn.connect_exe.c_str(), conn.connect_exe.c_str() + s, NULL ); //--------todo: args?
        }
        sleep( 15 );
        res_init();
    }
    //while( true ) end

    if( s_cm_pid != -1 )
    {
        kill( s_cm_pid, 15 );
        s_cm_pid = -1;

        LogOutLine( "Connect exe quit." );
    }

    DirectOutLine( "Mnets quit." );
    CloseLogFile();
    return 0;
}

