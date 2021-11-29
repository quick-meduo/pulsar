/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#include <signal.h>
#include <stdio.h>
#include <stdarg.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <getopt.h>
#include <nlohmann/json.hpp>
#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <sstream>

using nlohmann::json;


#include <pulsar/Client.h>
#include <lib/LogUtils.h>

DECLARE_LOG_OBJECT()

using namespace pulsar;

std::string    pipe_file_name = "";
bool           enabeDebug = false;
std::string    pulsar_url = "";


std::string& ltrim(std::string &s)
{
    auto it = std::find_if(s.begin(), s.end(),
                           [](char c) {
                               return !std::isspace<char>(c, std::locale::classic());
                           });
    s.erase(s.begin(), it);
    return s;
}

std::string& rtrim(std::string &s)
{
    auto it = std::find_if(s.rbegin(), s.rend(),
                           [](char c) {
                               return !std::isspace<char>(c, std::locale::classic());
                           });
    s.erase(it.base(), s.end());
    return s;
}

bool contains(std::string s, std::string sub){
    return s.find(sub) != std::string::npos;
}

std::string& trim(std::string &s) {
    return ltrim(rtrim(s));
}

bool startsWith(std::string s, std::string sub){
    return s.find(sub)==0? true:false;
}

bool endsWith(std::string s,std::string sub){
    return s.rfind(sub)==(s.length()-sub.length())?true:false;
}

std::string compress(const std::string &data) {
    boost::iostreams::filtering_streambuf<boost::iostreams::output> output_stream;
    output_stream.push(boost::iostreams::gzip_compressor());
    std::stringstream string_stream;
    output_stream.push(string_stream);
    boost::iostreams::copy(boost::iostreams::basic_array_source<char>(data.c_str(),
                                                                      data.size()), output_stream);
    return string_stream.str();
}

std::string decompress(const std::string &cipher_text) {
    std::stringstream string_stream;
    string_stream << cipher_text;
    boost::iostreams::filtering_streambuf<boost::iostreams::input> input_stream;
    input_stream.push(boost::iostreams::gzip_decompressor());

    input_stream.push(string_stream);
    std::stringstream unpacked_text;
    boost::iostreams::copy(input_stream, unpacked_text);
    return unpacked_text.str();
}

static void
sigHandler(int sig)
{
    static int count = 0;

    if (sig == SIGINT) {
        count++;
        printf("Caught SIGINT (%d)\n", count);

        if(count > 2){
            exit(EXIT_SUCCESS);
        }
        return;
    }

    printf("Caught SIGQUIT - Exited!\n");
    exit(EXIT_SUCCESS);
}

void PrintHelp()
{
    std::cout <<
            "-p, --pipe <name>:       Input pipe name\n"
            "-d, --debug:             Enable debug\n"
            "-m, --pulsar <url>:      Set pulsar MQ address\n"
            "h|?, --help:              Show help\n";
    exit(1);
}

void ProcessArgs(int argc, char** argv)
{
    const char* const short_opts = "p:dm:h";
    const option long_opts[] = {
            {"pipe", required_argument, nullptr, 'p'},
            {"debug", required_argument, nullptr, 'd'},
            {"pulsar", required_argument, nullptr, 'm'},
            {"help", no_argument, nullptr, 'h'},
            {nullptr, no_argument, nullptr, 0}
    };

    while (true)
    {
        const auto opt = getopt_long(argc, argv, short_opts, long_opts, nullptr);

        if (-1 == opt)
            break;

        switch (opt)
        {
        case 'p':
            pipe_file_name = std::string(optarg);
            break;
        case 'd':
            enabeDebug = true;
            break;
        case 'm':
            pulsar_url = std::string(optarg);
            break;
        case 'h': // -h or --help
        case '?': // Unrecognized option
        default:
            PrintHelp();
            break;
        }
    }
}

int main(int argc, char* args[]) {
    if (signal(SIGINT, sigHandler) == SIG_ERR){
        exit(1);
    }
        
    if (signal(SIGQUIT, sigHandler) == SIG_ERR){
        exit(1);
    }

    ProcessArgs(argc,args);

    if(pipe_file_name == ""){
       PrintHelp();
    }

    if(pulsar_url == ""){
       PrintHelp();
    }

    Client client(pulsar_url);

    Producer producer;
    Result result = client.createProducer("non-persistent://public/default/my-topic", producer);
    if (result != ResultOk) {
        LOG_ERROR("Error creating producer: " << result);
        return -1;
    }

    //initialize pipeline
    std::ifstream pipein(pipe_file_name);
    std::stringstream sstreamin;
    std::string packet;
    std::vector<std::string> stack;

    while(true) 
    {
        while(std::getline(pipein, packet)){
            packet = trim(packet);
            
            if(startsWith(packet,"//")){
                continue;
            }

            if(startsWith(packet,"{") && stack.empty()){
                size_t n = std::count(packet.begin(), packet.end(), '{');
                for(int i=0;i<n;i++){
                   stack.push_back("{");
                }
            } else if(contains(packet,"{") && !stack.empty()){
                size_t n = std::count(packet.begin(), packet.end(), '{');
                for(int i=0;i<n;i++){
                   stack.push_back("{");
                }
            }

            if(enabeDebug) {
                LOG_INFO("Recved: " << packet <<"\n"); 
                LOG_INFO("Status: " << stack.size() << " | " << sstreamin.str().size() <<"\n"); 
            }

            if(stack.empty()) {
                sstreamin.clear();
                sstreamin.str("");
                continue;
            }

            if(sstreamin.str().size() > 200000){
                sstreamin.clear();
                sstreamin.str("");
                stack.clear();
            }

            sstreamin << trim(packet) << std::endl;

            if(contains(packet,"}")){
                size_t n = std::count(packet.begin(), packet.end(), '}');
                for(int i=0;i<n;i++){
                   if(stack.empty()){
                        sstreamin.clear();
                        sstreamin.str("");
                        break;
                   } else {
                        stack.pop_back();
                        if(stack.empty()){
                            std::string json_packet = sstreamin.str();
                            if(endsWith(json_packet,"},\n")) {
                                json_packet = json_packet.substr(0, json_packet.size()-2);
                            }
                            Message msg = MessageBuilder().setContent(compress(json_packet)).build();
                            // Message msg = MessageBuilder().setContent((json_packet)).build();
                            Result  res = producer.send(msg);
                            
                            if(enabeDebug){
                                if(json_packet.length() > 100){
                                   LOG_INFO("Message sent: " << res << "||\n" << json_packet.substr(0,100) << "\n ..... \n"<<json_packet.substr(json_packet.length()-100,json_packet.length())<< "\n");
                                } else {
                                   LOG_INFO("Message sent: " << res << "||\n" << json_packet << "\n"); 
                                }
                            }

                            sstreamin.clear();
                            sstreamin.str("");
                        }
                    }
                }
            }
        }
    }
    
    client.close();
    // atexit(client.close());
}
