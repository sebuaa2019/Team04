/*********************************************************************
* Software License Agreement (BSD License)
* 
*  Copyright (c) 2017-2020, Waterplus http://www.6-robot.com
*  All rights reserved.
* 
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
* 
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the WaterPlus nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
* 
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  FOOTPRINTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/
/* @author Zhang Wanjie                                             */
#include <ros/ros.h>
#include <std_msgs/String.h>
#include "wpb_home_tutorials/Follow.h"
#include <geometry_msgs/Twist.h>
#include "xfyun_waterplus/IATSwitch.h"
#include <sound_play/SoundRequest.h>
#include "wpb_home_tutorials/Follow.h"
#include <move_base_msgs/MoveBaseAction.h>
#include <actionlib/client/simple_action_client.h>
#include <waterplus_map_tools/Waypoint.h>
#include <waterplus_map_tools/GetWaypointByName.h>
#include <tf/transform_listener.h>
#include <geometry_msgs/PoseStamped.h>

#include <cv_bridge/cv_bridge.h>
#include <image_transport/image_transport.h>
#include <opencv2/highgui/highgui.hpp>
#include "opencv2/imgproc/imgproc.hpp"

#include <cstdlib>
#include <string>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <fstream>

#include "../include/rosvis/gui.h"

using namespace std;

#define STATE_READY     0
#define STATE_FOLLOW    1
#define STATE_ASK       2
#define STATE_GOTO      3
#define STATE_GRAB      4
#define STATE_COMEBACK  5
#define STATE_PASS      6
#define STATE_WAIT      7

typedef actionlib::SimpleActionClient<move_base_msgs::MoveBaseAction> MoveBaseClient;
static string strGoto;
static sound_play::SoundRequest spk_msg;
static ros::Publisher spk_pub;
static ros::Publisher vel_pub;
static string strToSpeak = "";
static string strKeyWord = "";
static ros::ServiceClient clientIAT;
static xfyun_waterplus::IATSwitch srvIAT;
static ros::ServiceClient cliGetWPName;
static waterplus_map_tools::GetWaypointByName srvName;
static ros::Publisher add_waypoint_pub;
static ros::ServiceClient follow_start;
static ros::ServiceClient follow_stop;
static ros::ServiceClient follow_resume;
static wpb_home_tutorials::Follow srvFlw;
static ros::Publisher behaviors_pub;
static std_msgs::String behavior_msg;

static ros::Subscriber grab_result_sub;
static ros::Subscriber pass_result_sub;
static bool bGrabDone;
static bool bPassDone;

static int nState = STATE_READY;
static int nDelay = 0;

static vector<string> arKeyword;

// 添加航点关键词
void InitKeyword()
{
    arKeyword.push_back("start");   //机器人开始启动的地点,最后要回去
    arKeyword.push_back("record");
    arKeyword.push_back("Record");
}

// 从句子里找arKeyword里存在的关键词
static string FindKeyword(string inSentence)
{
    string res = "";
    int nSize = arKeyword.size();
    for(int i=0;i<nSize;i++)
    {
        int nFindIndex = inSentence.find(arKeyword[i]);
        if(nFindIndex >= 0)
        {
            res = arKeyword[i];
            break;
        }
    }
    return res;
}

// 将机器人当前位置保存为新航点
void AddNewWaypoint(string inStr)
{
    tf::TransformListener listener;
    tf::StampedTransform transform;
    try
    {
        listener.waitForTransform("/map","/base_footprint",  ros::Time(0), ros::Duration(10.0) );
        listener.lookupTransform("/map","/base_footprint", ros::Time(0), transform);
    }
    catch (tf::TransformException &ex) 
    {
        ROS_ERROR("[lookupTransform] %s",ex.what());
        return;
    }

    float tx = transform.getOrigin().x();
    float ty = transform.getOrigin().y();
    tf::Stamped<tf::Pose> p = tf::Stamped<tf::Pose>(tf::Pose(transform.getRotation() , tf::Point(tx, ty, 0.0)), ros::Time::now(), "map");
    geometry_msgs::PoseStamped new_pos;
    tf::poseStampedTFToMsg(p, new_pos);

    waterplus_map_tools::Waypoint new_waypoint;
    new_waypoint.name = inStr;
    new_waypoint.pose = new_pos.pose;
    add_waypoint_pub.publish(new_waypoint);

    ROS_WARN("[New Waypoint] %s ( %.2f , %.2f )" , new_waypoint.name.c_str(), tx, ty);
}

// 语音说话
void Speak(string inStr)
{
    spk_msg.arg = inStr;
    spk_pub.publish(spk_msg);
}

// 跟随模式开关
static void FollowSwitch(bool inActive, float inDist)
{
    if(inActive == true)
    {
        srvFlw.request.thredhold = inDist;
        if (!follow_start.call(srvFlw))
        {
            ROS_WARN("[CActionManager] - follow start failed...");
        }
    }
    else
    {
        if (!follow_stop.call(srvFlw))
        {
            ROS_WARN("[CActionManager] - failed to stop following...");
        }
    }
}

// 物品抓取模式开关
static void GrabSwitch(bool inActive)
{
    if(inActive == true)
    {
        behavior_msg.data = "grab start";
        behaviors_pub.publish(behavior_msg);
    }
    else
    {
        behavior_msg.data = "grab stop";
        behaviors_pub.publish(behavior_msg);
    }
}

// 物品递给开关
static void PassSwitch(bool inActive)
{
    if(inActive == true)
    {
        behavior_msg.data = "pass start";
        behaviors_pub.publish(behavior_msg);
    }
    else
    {
        behavior_msg.data = "pass stop";
        behaviors_pub.publish(behavior_msg);
    }
}

// 语音识别结果处理函数
void KeywordCB(const std_msgs::String::ConstPtr & msg)
{
    ROS_WARN("------ Keyword = %s ------",msg->data.c_str());
    static int failCount = 0;
    if(nState == STATE_FOLLOW)
    {
        // 从识别结果句子中查找物品（航点）关键词
        string strKeyword = FindKeyword(msg->data);
        int nLenOfKW = strlen(strKeyword.c_str());
        if(nLenOfKW > 0)
        {
            FILE* python_exec = popen("python3 /home/robot/catkin_ws/src/wpb_home_apps/src/ImageRecognition/sdk_ImageRecognition.py", "r");
            char keyword[1024], bash_output[1024]; //设置一个合适的长度，以存储每一行输出

            ifstream in("/home/robot/team104_temp/IR_res.txt");
            if (in.is_open() && !in.eof())
                in >> keyword;
            in.close();
            string objectName(keyword);
            arKeyword.push_back(objectName);

            fgets(bash_output, sizeof(bash_output), python_exec);
            cout << bash_output;
            
            // 发现物品（航点）关键词
            
            ROS_WARN("[[[%s]]]", keyword);
            if (objectName != "Fruit" && objectName != "Bottle")
            {
            	failCount ++;
            	if (failCount <= 1)
            	{
            		string strSpeak = "Unexpected object, try again, please"; 
	            	Speak(strSpeak);
            	} else 
            	{
            		string strSpeak = "Unexpected object and tried for two times. Memoried as Unknown."; 
	            	Speak(strSpeak);
	            	AddNewWaypoint("Unknown");
	            	failCount = 0;
            	}
	            
            } else
            {
		    	AddNewWaypoint(objectName);
	            string strSpeak = objectName + " . OK. I have memoried. Next one , please"; 
	            Speak(strSpeak);
	            failCount = 0;
            }
        }

        // 停止跟随的指令
        int nFindIndex = msg->data.find("top");
        if(nFindIndex >= 0)
        {
            FollowSwitch(false, 0);
            AddNewWaypoint("master");
        //     // nState = STATE_ASK;
            nState = STATE_WAIT;
            nDelay = 0;
        }
    }

    if(nState == STATE_ASK)
    {
        // 从识别结果句子中查找物品（航点）关键词
        string strKeyword = FindKeyword(msg->data);
        int nLenOfKW = strlen(strKeyword.c_str());
        if(nLenOfKW > 0)
        {
            // 发现物品（航点）关键词
            strGoto = strKeyword;
            string strSpeak = strKeyword + " . OK. I will go to get it for you."; 
            Speak(strSpeak);
            nState = STATE_GOTO;
        }
    }
}


void ProcColorCB(const sensor_msgs::ImageConstPtr& msg)
{
    //ROS_INFO("ProcColorCB");
    cv_bridge::CvImagePtr cv_ptr;
    try
    {
        cv_ptr = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);
    }
    catch (cv_bridge::Exception& e)
    {
        ROS_ERROR("cv_bridge exception: %s", e.what());
        return;
    }
    int res = imwrite("/home/robot/team104_temp/image.bmp",cv_ptr->image);
    // cout << "XXXXXXXXXXXXXXXXXX" << endl;
    // cout << res << endl;
}

// 物品抓取状态
void GrabResultCallback(const std_msgs::String::ConstPtr& res)
{
    int nFindIndex = 0;
    nFindIndex = res->data.find("done");
    if( nFindIndex >= 0 )
    {
        bGrabDone = true;
    }
}

// 物品递给状态
void PassResultCallback(const std_msgs::String::ConstPtr& res)
{
    int nFindIndex = 0;
    nFindIndex = res->data.find("done");
    if( nFindIndex >= 0 )
    {
        bPassDone = true;
    }
}

int main(int argc, char** argv)
{
    startGUI(argc, argv);
    ros::init(argc, argv, "wpb_home_shopping");

    ros::NodeHandle n;
    ros::Subscriber sub_sr = n.subscribe("/xfyun/iat", 10, KeywordCB);
    ros::Subscriber rgb_sub = n.subscribe("/kinect2/qhd/image_color", 1 , ProcColorCB);
    follow_start = n.serviceClient<wpb_home_tutorials::Follow>("wpb_home_follow/start");
    follow_stop = n.serviceClient<wpb_home_tutorials::Follow>("wpb_home_follow/stop");
    follow_resume = n.serviceClient<wpb_home_tutorials::Follow>("wpb_home_follow/resume");
    cliGetWPName = n.serviceClient<waterplus_map_tools::GetWaypointByName>("/waterplus/get_waypoint_name");
    add_waypoint_pub = n.advertise<waterplus_map_tools::Waypoint>( "/waterplus/add_waypoint", 1);
    spk_pub = n.advertise<sound_play::SoundRequest>("/robotsound", 20);
    spk_msg.sound = sound_play::SoundRequest::SAY;
    spk_msg.command = sound_play::SoundRequest::PLAY_ONCE;
    vel_pub = n.advertise<geometry_msgs::Twist>("/cmd_vel", 10);
    clientIAT = n.serviceClient<xfyun_waterplus::IATSwitch>("xfyun_waterplus/IATSwitch");
    behaviors_pub = n.advertise<std_msgs::String>("/wpb_home/behaviors", 30);
    grab_result_sub = n.subscribe<std_msgs::String>("/wpb_home/grab_result",30,&GrabResultCallback);
    pass_result_sub = n.subscribe<std_msgs::String>("/wpb_home/pass_result",30,&PassResultCallback);

    InitKeyword();

    ROS_WARN("[main] wpb_home_shopping");
    ros::Rate r(30);
    while(ros::ok())
    {
        if (nState == STATE_WAIT)
        {
            int count = 0;
            ifstream in("/home/robot/team104_temp/status.txt");
            if (in.is_open())
            { 
                if (!in.eof())
                {
                    char buffer[20];
                    in >> buffer;
                    if (buffer[0] == '1' && count == 0)
                    {
                        nState = STATE_FOLLOW;
                        Speak("SLAM ON");
                        ROS_WARN("SLAM ON");          
                        count++;              
                    }
                    else if (buffer[0] == '2')
                    {
                        nState = STATE_ASK;
                        Speak("OK. What do you want me to fetch?");
                        ROS_WARN("ASK");
                    }
                }    
            }
            
        }

        // 1、刚启动，准备
        if(nState == STATE_READY)
        {
            // 启动后延迟一段时间然后开始跟随
            nDelay ++;
            // ROS_WARN("[STATE_READY] - nDelay = %d", nDelay);
            if(nDelay > 100)
            {
                AddNewWaypoint("start");
                nDelay = 0;
                // nState = STATE_FOLLOW;
                nState = STATE_WAIT;
            }
        }

        // 2、跟随阶段
        if(nState == STATE_FOLLOW)
        {
            if(nDelay == 0)
            {
               FollowSwitch(false, 0.7);
            }
            nDelay ++;
        }

        // 3、询问要去哪个航点
        if(nState == STATE_ASK)
        {
            
        }

        // 4、导航去指定航点
        if(nState == STATE_GOTO)
        {
            srvName.request.name = strGoto;
            if (cliGetWPName.call(srvName))
            {
                std::string name = srvName.response.name;
                float x = srvName.response.pose.position.x;
                float y = srvName.response.pose.position.y;
                ROS_INFO("[STATE_GOTO] Get_wp_name = %s (%.2f,%.2f)", strGoto.c_str(),x,y);

                MoveBaseClient ac("move_base", true);
                if(!ac.waitForServer(ros::Duration(5.0)))
                {
                    ROS_INFO("The move_base action server is no running. action abort...");
                }
                else
                {
                    move_base_msgs::MoveBaseGoal goal;
                    goal.target_pose.header.frame_id = "map";
                    goal.target_pose.header.stamp = ros::Time::now();
                    goal.target_pose.pose = srvName.response.pose;
                    ac.sendGoal(goal);
                    ac.waitForResult();
                    if(ac.getState() == actionlib::SimpleClientGoalState::SUCCEEDED)
                    {
                        ROS_INFO("Arrived at %s!",strGoto.c_str());
                        Speak("OK. I am taking it.");
                        nState = STATE_GRAB;
                        nDelay = 0;
                    }
                    else
                    {
                        ROS_INFO("Failed to get to %s ...",strGoto.c_str() );
                        Speak("Failed to go to the waypoint.");
                        nState = STATE_ASK;
                    }
                }
                
            }
            else
            {
                ROS_ERROR("Failed to call service GetWaypointByName");
                Speak("There is no this waypoint.");
                nState = STATE_ASK;
            }
        }

        // 5、抓取物品
        if(nState == STATE_GRAB)
        {
            if(nDelay == 0)
            {
                bGrabDone = false;
                GrabSwitch(true);
            }
            nDelay ++;
            if(bGrabDone == true)
            {
                GrabSwitch(false);
                Speak("I got it. I am coming back.");
                nState = STATE_COMEBACK;
            }
        }

        // 6、抓完物品回来
        if(nState == STATE_COMEBACK)
        {
            srvName.request.name = "master";
            if (cliGetWPName.call(srvName))
            {
                std::string name = srvName.response.name;
                float x = srvName.response.pose.position.x;
                float y = srvName.response.pose.position.y;
                ROS_INFO("[STATE_COMEBACK] Get_wp_name = %s (%.2f,%.2f)", strGoto.c_str(),x,y);

                MoveBaseClient ac("move_base", true);
                if(!ac.waitForServer(ros::Duration(5.0)))
                {
                    ROS_INFO("The move_base action server is no running. action abort...");
                }
                else
                {
                    move_base_msgs::MoveBaseGoal goal;
                    goal.target_pose.header.frame_id = "map";
                    goal.target_pose.header.stamp = ros::Time::now();
                    goal.target_pose.pose = srvName.response.pose;
                    ac.sendGoal(goal);
                    ac.waitForResult();
                    if(ac.getState() == actionlib::SimpleClientGoalState::SUCCEEDED)
                    {
                        ROS_INFO("Arrived at %s!",strGoto.c_str());
                        Speak("Hi,master. This is what you wanted.");
                        nState = STATE_PASS;
                        nDelay = 0;
                    }
                    else
                    {
                        ROS_INFO("Failed to get to %s ...",strGoto.c_str() );
                        Speak("Failed to go to the master.");
                        nState = STATE_ASK;
                    }
                }
                
            }
            else
            {
                ROS_ERROR("Failed to call service GetWaypointByName");
                Speak("There is no waypoint named master.");
                nState = STATE_ASK;
            }
        }

        // 7、将物品给主人
        if(nState == STATE_PASS)
        {
            if(nDelay == 0)
            {
                bPassDone = false;
                PassSwitch(true);
            }
            nDelay ++;
            if(bPassDone == true)
            {
                PassSwitch(false);
                Speak("OK. What do you want next?");
                nState = STATE_ASK;
                // nState = STATE_WAIT;
            }
        }
        
        ros::spinOnce();
        r.sleep();
    }

    return 0;
}
