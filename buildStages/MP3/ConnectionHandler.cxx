#include <sys/un.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <resolv.h>
#include <unistd.h>
#include <pthread.h>
#include <list>
#include <sys/stat.h>
#include <math.h>
#include <fstream>
#include <signal.h>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/iostreams/stream_buffer.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/mpl/inserter.hpp>
#include <boost/iostreams/filtering_stream.hpp>

#include "ConnectionHandler.h"
#include "Timer.h"
#include "membershipList.h"
#include "logger.h"
#include "utility.h"
#include "coordinator.h"

#define BUFLEN 10000
#define SERVER_PORT 45000
#define FILEPATH "/tmp/ag/peers.dump"
#define FRACTION 0.5
//#define MASTER "192.168.159.133"

using namespace std;
using namespace P2P;

//std::stringstream ss; 
pthread_mutex_t mutexsum;
bool leave;
long int leaveTimeStamp;
static long int sends = 1;
ErrorLog *logger;

ConnectionHandler::ConnectionHandler(string src,
                                     int machineno,
                                     std::list<string> dest,
                                     int sendPercentage,
                                     float interval):Timer(interval)
{
    struct sockaddr_in my_addr;
    mystruct* udp = new mystruct;
    mystruct* tcp = new mystruct;
    mystruct* show = new mystruct;

    int udpSock;
    int tcpSock;
    int err;

    pthread_t thread_id=0;
    string address;
    string port;
    std::size_t pos;

    address = src;
    machine_no = machineno;
    sendPct = sendPercentage;

    try
    {
        /* Create my own membership list
           Initialize the logger 
         */
        string netId = MembershipList::getNetworkID(address);
        logger = new ErrorLog(machine_no);
        //MembershipList *memList = new MembershipList(machine_no, netId, logger);
        Coordinator *coord = new Coordinator();
        KeyValueStore *keyValueStore1 = new KeyValueStore(machine_no, logger, coord);
        MembershipList *memList = new MembershipList(machine_no, netId, logger, coord);
        this->setCoordinatorPtr(coord);
        this->setMemPtr(memList);
        this->setKeyValuePtr(keyValueStore1);
        myIP = src;
        leave = false;
        
        /* Initialize signal handler */
        struct sigaction sigAct;
        memset(&sigAct, 0, sizeof(sigAct));
        sigAct.sa_handler = ConnectionHandler::sendLeaveMsg;
        sigaction(SIGTERM, &sigAct, 0);

        /* Opening a udp socket for membershiplist exchange */
        udpSock = Utility::udpSocket(address,SERVER_PORT);
        //if(udpSock == -1)

        /* Opening a tcp socket for key value store operation */
        tcpSock = Utility::tcpSocket(address,SERVER_PORT);
            
        /* Create threads to start listening 
           to updates from other members
         */
        udp->sock = udpSock;
        udp->owner = this;

        tcp->orgsock = tcpSock;
        tcp->owner = this;
        show->owner = this;
        
        pthread_create(&thread_id,0,&ConnectionHandler::UDPSocketHandler, (void*)udp);
        pthread_detach(thread_id);
        pthread_create(&thread_id,0,&ConnectionHandler::TCPSocketHandler, (void*)tcp);
        pthread_detach(thread_id);
        pthread_create(&thread_id,0,&ConnectionHandler::showCommandPrompt, (void*)tcp);
        pthread_detach(thread_id);

        /* Add a timer task to send membership list
           at regular intervals
         */
        this->addTask(this);
    }
    catch(string sockException)
    {
        close(udpSock);
        udpSock = -1;
        exit(-1);
    }
    
}

void* ConnectionHandler::showCommandPrompt(void* lp)
{
    mystruct *ptr = static_cast<mystruct*>(lp);
    ConnectionHandler *ptr1 = (ConnectionHandler*)ptr->owner;

    string cmdToSend;
    int errCode;
    cmdToSend = CommandLineTools::showAndHandlePrompt("1");

    while(cmdToSend != "exit") {
        KeyValueStoreCommand command = CommandLineTools::parseKeyValueStoreCmd(cmdToSend);

        if(command.isValidCommand()) {
            string msg;
            msg = "Key value store entries:\n";
            msg += ptr1->getKeyValuePtr()->returnAllEntries(&errCode);
            msg += "Membership list:\n";
            msg += ptr1->getMemPtr()->getkeyToIPMapDetails();
            cout << msg << endl;
        } else {
            cout<<"Malformed command!"<<endl;
        }

        cmdToSend = CommandLineTools::showAndHandlePrompt("1");
    }
    pthread_exit(NULL);
}

void* ConnectionHandler::TCPSocketHandler(void* lp)
{
    mystruct *ptr = static_cast<mystruct*>(lp);
    ConnectionHandler *ptr1 = (ConnectionHandler*)ptr->owner;
    socklen_t addr_size = 0;
    int* csock;
    sockaddr_in sadr;
    pthread_t thread_id=0;

    while(true)
    {
        /* accept connection from peers for
         * exchange of key and value pairs
         */
        csock = (int*)malloc(sizeof(int));
        if((*csock = accept(ptr->orgsock, (sockaddr*)&sadr, &addr_size))!= -1)
        {
            mystruct *ptrToSend = new mystruct;
            ptrToSend->owner = ptr->owner;
            //ptrToSend->orgsock = ptr->orgsock;
            ptrToSend->sock = *csock;
            pthread_create(&thread_id,0,&ConnectionHandler::updateKeyValue,(void*)ptrToSend);
            pthread_detach(thread_id);
        }
        else
        {
            cout << "Error accepting connection " << strerror(errno) << endl;
        }
    }
}

void* ConnectionHandler::updateKeyValue(void* lp)
{
    mystruct *ptr = static_cast<mystruct*>(lp);
    ConnectionHandler *ptr1 = (ConnectionHandler*)ptr->owner;
    char buffer[12288];
    int buffer_len = 12288;
    string msg;
    int bytecount;
    long int keyToInsert;
    int hash;

    memset(buffer, 0, buffer_len);
    /* Accept the key vlaue store command */
    if((bytecount = recv(ptr->sock, buffer, buffer_len, 0))== -1)
    {
        cout << "Error receiving data " << strerror(errno) << endl;
    }
    string received = buffer;
    /* Parse and find out the command type */
    KeyValueStoreCommand command = CommandLineTools::parseKeyValueStoreCmd(received);
    
    string ip;
    if(command.getOperation() != SHOW_KVSTORE) {
        /* Find the key */
        keyToInsert = command.getKey();
        /* Calculate the key hash */
        hash = Hash::calculateKeyHash(keyToInsert);
        /* Find the IP of the node where the key value pair should reside */
        ip = ptr1->getMemPtr()->getIPToSendToFromKeyHash(hash);
    } else {
        ip = ptr1->myIP;
    }

    cout<<"Operation is "
        << command.getOperation()
        << ". Key is "
        << command.getKey()
        << ". Value is "
        <<command.getValue()
        << ". Key Hash is "
        << Hash::calculateKeyHash(keyToInsert)
        <<endl;


    if(ptr1->myIP == ip)
    {
        /* If IP same as my IP 
         * Perform the necessary operation 
         * on the local node */
        int errCode = 0;
        int status = SUCCESS;
        
        cout << "Correct node found, performing " << command.getOperation() << " locally" << endl;
        if(command.getOperation() == INSERT_KEY) {
            status = ptr1->getKeyValuePtr()->insertKeyValue(command.getKey(), command.getValue(), &errCode);
        } else if(command.getOperation() == DELETE_KEY) {
            status = ptr1->getKeyValuePtr()->deleteKey(command.getKey(), &errCode);
        } else if(command.getOperation() == UPDATE_KEY) {
            status = ptr1->getKeyValuePtr()->updateKeyValue(command.getKey(), command.getValue(), &errCode);
        } else if(command.getOperation() == LOOKUP_KEY) {
            msg = ptr1->getKeyValuePtr()->lookupKey(command.getKey(), &errCode);
        } else if(command.getOperation() == SHOW_KVSTORE) {
            msg = "Key value store entries:\n";
            msg += ptr1->getKeyValuePtr()->returnAllEntries(&errCode);
            msg += "Membership list:\n";
            msg += ptr1->getMemPtr()->getkeyToIPMapDetails();
        }

        if(command.getOperation() != LOOKUP_KEY && command.getOperation() != SHOW_KVSTORE) {
            msg = status == SUCCESS ? "Command succeeded" : "Command failed";
        }
    }    
    else
    {
        /* If the IP is different, connect to the correct node */
        cout << "Key hash higher than my Node Hash.. Connecting to the right node with ip" 
             << ip 
             << endl;
        msg = Utility::tcpConnectSocket(ip,SERVER_PORT,received);
    }
    strcpy(buffer,msg.c_str());
    buffer[strlen(buffer)]='\0';
    /* reply back with success or failure, or the key value pair in case of a show command */
    if(send(ptr->sock, buffer, strlen(buffer), 0) < 0)
    {
        cout << "ERROR: Failed to send file size" << strerror(errno) << endl;
    }
    close(ptr->sock);
    ptr->sock = -1;
    pthread_exit(NULL);
}

void* ConnectionHandler::UDPSocketHandler(void* lp)
{
    mystruct *ptr = static_cast<mystruct*>(lp);
    ConnectionHandler *ptr1 = (ConnectionHandler*)ptr->owner;

    struct sockaddr_in cli_addr;
    int sockfd, i; 
    socklen_t slen=sizeof(cli_addr);
    //string recvstr;
    int byte_count;
    char buf[1024];
    pthread_t thread_id=0;
    /* Initialize the mutex */
    pthread_mutex_init(&mutexsum, NULL);
    
    while(1)
    {
        try
        {
            /* receive data from other members */
            memset(buf, 0, 1024);
            byte_count = recvfrom(ptr->sock, buf, 1024, 0, (struct sockaddr*)&cli_addr, &slen);
            if(byte_count == -1)
            {
                string msg = "Failed to receive on socket";
                int errCode = 0;
                logger->logError(SOCKET_ERROR, msg , &errCode);
                //cout << "Error accepting connection " << strerror(errno) << endl;
            }
            /* deserialze the received membership list */
            string recvstr(buf);
            std::stringstream ss1; 
            ss1 << recvstr;
    
            boost::archive::text_iarchive ia(ss1);
            MembershipList recvList;
            ia >> recvList;
            int errorcode = 0;
            ss1.clear();
    
            MembershipList *list = new MembershipList();
            *list = recvList;
            mystruct *ptrToSend = new mystruct;
            ptrToSend->owner = ptr->owner;
            ptrToSend->sock = ptr->sock;
            ptrToSend->mPtr = list;

            /* Thread to update my membership list
               and add new members */
            pthread_create(&thread_id,0,&ConnectionHandler::updateMembershipList, (void*)ptrToSend);
            pthread_detach(thread_id);
        }
        catch(exception& e)
        {
            string msg = "Failed to de-serialize";
            int errCode = 0;
            logger->logError(SERIALIZATION_ERROR, msg , &errCode);
        }
    }
    close(ptr->sock);
    ptr->sock = -1;
    pthread_exit(NULL);
}

void* ConnectionHandler::updateMembershipList(void* lp)
{
    mystruct *ptr = static_cast<mystruct*>(lp);
    ConnectionHandler *ptr1 = (ConnectionHandler*)ptr->owner;
    int errorcode = 0;

    /* Take a lock on the membership list */
    pthread_mutex_lock (&mutexsum);
    //ptr->mPtr->printMemList();
    ptr1->getMemPtr()->updateMembershipList(ptr->mPtr,&errorcode);
    /* Release the lock after updating the membership list */
    pthread_mutex_unlock (&mutexsum);

    delete ptr->mPtr;
    delete ptr;
    pthread_exit(NULL);
}

void ConnectionHandler::executeCb()
{
    int errorcode = 0;
    vector<string> memberIPs;
    
    /* Task called after every regular interval
       Send membership list to a fraction of machines in my list
     */

    this->getMemPtr()->getListOfMachinesToSendTo(FRACTION, 
                                                 &memberIPs, 
                                                 &errorcode);

    //cout << "Sending to : " << memberIPs.size() << endl;
    /* Set the leave bit if SIGTERM received */
    if(leave)
    {
        this->getMemPtr()->requestRetirement(&errorcode);
    }
    sendMemberList(memberIPs);
}

void ConnectionHandler::sendMemberList(vector<string> memberIPs)
{
    struct sockaddr_in serv_addr;
    int sockfd, i, slen=sizeof(serv_addr);
    char buf[BUFLEN];
    int errorcode = 0;
    std::stringstream ss;
#ifdef TESTING
    int calPct = rand() % 100;
    if(sendPct > 0 && (calPct < sendPct))
#endif
    {
        /* Increment heartbeat */
    	this->getMemPtr()->incrementHeartbeat(1,&errorcode);
        /* Call process list to check for timeouts */
    	if(!leave) {
            this->getMemPtr()->processList(&errorcode);
    	}
        /* Check if a new member joined and if I have to move my keys to him */
        Coordinator *coord = this->getCoordinatorPtr();
        if(coord->hasMessage())
        {
            vector<string> commands;
            if(coord->getReason() == JOIN)
            {
                /* Find the IP of the node to move the keys to */
                commands = this->getKeyValuePtr()->getCommandsForJoin(&errorcode);
                string ip = this->getMemPtr()->getIPFromHash(coord->getNewMemberHash());
                cout << "Moving my keys to the new node in the system with IP " 
                     << ip 
                     << " and hash " << coord->getNewMemberHash() 
                     << endl;
                for(i = 0; i < commands.size() ; i++) {
                    Utility::tcpConnectSocket(ip,SERVER_PORT,commands[i]);
                }
                coord->setTransferKeysToMember(0);
            }
        }
        /* If time exceeds the time to cleanup exit the process */
    	if(leave && 
           ((time(0) - leaveTimeStamp) > this->getMemPtr()->timeToCleanupInSeconds()))
    	{
            //send the keys to another machine if I am leaving the system
            vector<string> commands = this->getKeyValuePtr()->getCommandsForLeave(&errorcode);
            string ip = this->getMemPtr()->getIPofSuccessor();
            for(i = 0; i < commands.size() ; i++) {
                Utility::tcpConnectSocket(ip,SERVER_PORT,commands[i]);
            }

            string msg = "Elvis has left the building";
            int errCode = 0;
            logger->logDebug(MEMBER_LEFT, msg , &errCode);
            //cout << "Clean up Time expired, Exiting now " << endl;
            exit(0);
    	}

        try
        {
            /* Serialize the membership list */
            boost::archive::text_oarchive oa(ss);
            oa << *(this->getMemPtr());
            std::string mystring(ss.str());


    	    if ((sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))==-1)
            {
                string msg = "Failed to open socket";
                int errCode = 0;
                logger->logError(SOCKET_ERROR, msg , &errCode);
                //cout << "Error opening socket" << strerror(errno) << std::endl;
            }
            /* Write current members to a file
               helps to find an initial list of members to talk to 
               if the master restarts
             */
    	    if(sends % 20 == 0 && machine_no == 1)
    	    {
    	        this->getMemPtr()->writeIPsToFile(&errorcode);
    	    }
    	    sends++;

    	    bzero(&serv_addr, sizeof(serv_addr));
    	    serv_addr.sin_family = AF_INET;
    	    serv_addr.sin_port = htons(45000);
            /* send membership list to list of member IPs */
    	    for(int i = 0; i < memberIPs.size(); i++)
    	    {
                //cout << "Sending  Machine " << machine_no << " Sending to IP " << memberIPs.at(i) << endl;
                serv_addr.sin_addr.s_addr = inet_addr(memberIPs.at(i).c_str());
                if (sendto(sockfd, mystring.c_str(), strlen(mystring.c_str()), 0, (struct sockaddr*)&serv_addr, slen)==-1)
                {
                    string msg = "Failed to send on socket";
                    int errCode = 0;
                    logger->logError(SOCKET_ERROR, msg , &errCode);
                }
    	    }
    	    close(sockfd);
        }
        catch(exception& e)
        {
            //Failed to serialize. Log it
            string msg = "Failed to serialize";
            int errCode = 0;
            logger->logError(SERIALIZATION_ERROR, msg , &errCode);
        }
    }
}

void ConnectionHandler::sendLeaveMsg(int signal)
{
    leave = true;    
    leaveTimeStamp = time(0);
}

ConnectionHandler::~ConnectionHandler()
{
}
