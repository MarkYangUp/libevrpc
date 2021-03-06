/***************************************************************************
 *
 * Copyright (c) 2016 aishuyu, Inc. All Rights Reserved
 *
 **************************************************************************/



/**
 * @file center_client_heartbeat.cpp
 * @author aishuyu(asy5178@163.com)
 * @date 2016/09/07 20:59:44
 * @brief
 *
 **/


#include "center_client_heartbeat.h"

#include <unistd.h>
#include <time.h>
#include <fstream>
#include <iostream>
#include <stdlib.h>
#include <sys/sysinfo.h>
#include <linux/kernel.h>

#include <google/protobuf/repeated_field.h>

#include "center_proto/center_cluster.pb.h"
#include "center_proto/center_client.pb.h"
#include "config_parser/config_parser.h"
#include "util/rpc_communication.h"
#include "util/rpc_util.h"

#define random(x) (rand()%x)

namespace libevrpc {

using std::string;
using namespace google::protobuf;

CenterClientHeartbeat::CenterClientHeartbeat(const string& config_file):
    config_file_(config_file),
    center_addrs_ptr_(new ADDRS_LIST_TYPE()),
    updatecenter_addrs_ptr_(new ADDRS_LIST_TYPE()),
    cluster_server_addrs_list_ptr_(new ADDRS_LIST_TYPE()),
    running_(false),
    center_port_(NULL) {
}

CenterClientHeartbeat::~CenterClientHeartbeat() {
    if (NULL != center_addrs_ptr_) {
        delete center_addrs_ptr_;
    }

    if (NULL != updatecenter_addrs_ptr_) {
        delete updatecenter_addrs_ptr_;
    }

    if (NULL != center_port_) {
        free(center_port_);
    }

    if (NULL != cluster_server_addrs_list_ptr_) {
        delete cluster_server_addrs_list_ptr_;
    }
}

void CenterClientHeartbeat::Run() {
    if (!InitCenterClientHB()) {
        fprintf(stderr, "Center Init HeartBeat failed!\n");
        return;
    }
    int32_t rc_size = updatecenter_addrs_ptr_->size();
    int32_t ca_size = center_addrs_ptr_->size();

    if (0 == rc_size || 0 == ca_size) {
        fprintf(stderr, "updated center list size is %d\n center addrs list size is %d\n", rc_size, ca_size);
        return;
    }

    while (running_) {
        ClientWithCenter cwc_proto;
        cwc_proto.set_client_center_action(UPDATE_SERVER_INFO);

        string cwc_str;
        if (!cwc_proto.SerializeToString(&cwc_str)) {
            fprintf(stderr, "SerializeToString the client to center error!\n");
            sleep(3);
            continue;
        }
        int32_t random_index = random(ca_size);
        int32_t conn_fd = TcpConnect(center_addrs_ptr_->at(random_index).c_str(), center_port_, 15);
        if (conn_fd <= 0) {
            close(conn_fd);
            fprintf(stderr, "Connect center failed!\n");
            sleep(20);
            continue;
        }

        if (RpcSend(conn_fd, CENTER2CLIENT, cwc_str) < 0) {
            close(conn_fd);
            fprintf(stderr, "Send info to center error!\n");
            sleep(10);
            continue;
        }

        string center_response_str;
        ClientWithCenter cwc_response_proto;
        if (RpcRecv(conn_fd, center_response_str, true) >= 0 ||
            cwc_response_proto.ParseFromString(center_response_str)) {
            const RepeatedPtrField<string>& center_list = cwc_response_proto.should_communicate_center();
            UpdateCenterAddrs(&center_list);
            const RepeatedPtrField<string>& server_list = cwc_response_proto.cluster_server_list();
            UpdateServerAddrs(&server_list);
        }
        close(conn_fd);
        sleep(5);
    }
}

void CenterClientHeartbeat::GetRpcServerList(ADDRS_LIST_TYPE& server_list) {
    ReadLockGuard wguard(servers_rwlock_);
    server_list.assign(cluster_server_addrs_list_ptr_->begin(), cluster_server_addrs_list_ptr_->end());
}

string CenterClientHeartbeat::RandomGetRpcServerAddr() {
    ReadLockGuard wguard(servers_rwlock_);
    int32_t random = random(cluster_server_addrs_list_ptr_->size());
    return cluster_server_addrs_list_ptr_->at(random);
}

bool CenterClientHeartbeat::InitCenterClientHB() {
    const string cfile = "/tmp/centers.data";
    int ftyp = access(cfile.c_str(), 0);
    if (0 != ftyp) {
        /*
         * 需要的服务器列表文件不存在，无法初始化后与其他机器进行通信
         * 启动失败!!!
         */
        fprintf(stderr, "Can't read center data file!\n");
        return false;
    }
    const char* center_port = ConfigParser::GetInstance(config_file_).IniGetString("rpc_client:port", "8899");
    strcpy(center_port_ = (char*)malloc(strlen(center_port) + 1), center_port);

    std::ifstream in(cfile);
    string line;

    const char* local_addr = GetLocalAddress();
    while (getline(in, line)) {
        // FOR Test
        if (strcmp(line.c_str(), local_addr) == 0) {
            continue;
        }
        center_addrs_ptr_->push_back(line);
    }

    int32_t ca_size = center_addrs_ptr_->size();
    if (0 == ca_size) {
        fprintf(stderr, "The center_addrs_ptr size is 0!\n");
        return false;
    }

    int32_t random_index = random(ca_size);
    int32_t conn_fd = TcpConnect(center_addrs_ptr_->at(random_index).c_str(), center_port_, 15);
    if (conn_fd <= 0) {
        fprintf(stderr, "Tcp connect %d failed\n", conn_fd);
        return false;
    }

    ClientWithCenter cwc_proto;
    cwc_proto.set_client_center_action(CLIENT_INIT_REQ);

    string cwc_str;
    if (!cwc_proto.SerializeToString(&cwc_str)) {
        close(conn_fd);
        fprintf(stderr, "cwc_proto serialize to string failed!\n");
        return false;
    }

    if (RpcSend(conn_fd, CENTER2CLIENT, cwc_str) < 0) {
        close(conn_fd);
        return false;
    }

    string center_response_str;
    if (RpcRecv(conn_fd, center_response_str, false) >= 0) {
        ClientWithCenter cwc_proto;
        if (cwc_proto.ParseFromString(center_response_str) &&
            cwc_proto.client_center_action() == CENTER_RESP_OK) {
            /*
             * 获取与当前Client对应Center服务器列表
             */
            const RepeatedPtrField<string>& center_list = cwc_proto.should_communicate_center();
            for (RepeatedPtrField<string>::const_iterator iter = center_list.begin();
                 iter != center_list.end();
                 ++iter) {
                updatecenter_addrs_ptr_->push_back(*iter);
            }
            /*
             * 第一次获取与当前Client对应的Cluster服务器列表
             */
            const RepeatedPtrField<string>& rpc_server_list = cwc_proto.cluster_server_list();
            for (RepeatedPtrField<string>::const_iterator iter = rpc_server_list.begin();
                 iter != rpc_server_list.end();
                 ++iter) {
                cluster_server_addrs_list_ptr_->push_back(*iter);
            }
        }
    }
    close(conn_fd);

    running_ = true;
    return true;
}

void CenterClientHeartbeat::UpdateCenterAddrs(const RepeatedPtrField<string>* center_list) {
    WriteLockGuard wguard(centers_rwlock_);
    for (RepeatedPtrField<string>::const_iterator iter = center_list->begin();
         iter != center_list->end();
         ++iter) {
        updatecenter_addrs_ptr_->push_back(*iter);
    }
}

void CenterClientHeartbeat::UpdateServerAddrs(const RepeatedPtrField<string>* server_list) {
    WriteLockGuard wguard(servers_rwlock_);
    for (RepeatedPtrField<string>::const_iterator iter = server_list->begin();
         iter != server_list->end();
         ++iter) {
        cluster_server_addrs_list_ptr_->push_back(*iter);
    }
}


}  // end of namespace libevrpc









/* vim: set expandtab ts=4 sw=4 sts=4 tw=100: */
