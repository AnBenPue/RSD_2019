#include <iostream>
#include <chrono>
#include <mutex>
#include "ros/ros.h"

#include "state.h"
#include <std_msgs/Int8.h>
#include <std_msgs/Int16.h>
#include <std_msgs/Empty.h>
#include <std_msgs/Bool.h>
#include <std_msgs/String.h>

//Services:
// #include "robot_control/goto_config.h"
#include <ur_dashboard_msgs/Load.h> //For loading UR programs
#include <std_srvs/Trigger.h> //For play, pause and stop programs
#include <ur_dashboard_msgs/GetProgramState.h>
#include "vision/check_brick.h"
#include "mes_ordering/DeleteOrder_srv.h"
#include "mes_ordering/GetOrder_srv.h"
#include "mir_api/mir_api_action.h"
#include "system_manager/get_packml_state.h"
#include "system_manager/get_feeder_status.h"
#include "ur_digital_ports/digitalOut_srv.h"

using namespace std;

#define BLUE_BRICKS   0
#define RED_BRICKS    1
#define YELLOW_BRICKS 2

#define BRICK_NOMATCH 0
#define BRICK_MATCH   1

#define FEEDER_WARNING_THRESH 5

#define FEEDER_MAX 32 //Max number of bricks in any feeder

#define MIR_CALL         1
#define MIR_POLL_ARRIVED 2
#define MIR_RELEASE      3
#define MIR_POLL_EXEC   10


//Estimate of feeder contents:
int feederEstimates[3] = {FEEDER_MAX, FEEDER_MAX, FEEDER_MAX};

//Runtime info:
int currentBox;
string currentBoxString;
bool running = true;
bool isPaused = false;
bool isStopped = true;
int bricksDiscarded;
ros::Time firstOrderStartTime;

//Order info:
int currentOrderContents[4][3]; //blue, red, yellow
int currentOrderContentsOrg[4][3]; //blue, red, yellow
bool boxContainsOrder[4] = {false, false, false, false};
int currentOrderIds[4];
string currentOrderTickets[4];
int currentOrderIdx;


//Callbacks:
void feederRefill(const std_msgs::Empty::ConstPtr& msg);
void pauseSystem(const std_msgs::Empty::ConstPtr& msg);
void playSystem(const std_msgs::Empty::ConstPtr& msg);
void stopSystem(const std_msgs::Empty::ConstPtr& msg);

//Robotics:
bool loadAndRunUrProgram(string filename);
ros::ServiceClient robotLoadProgram;
ur_dashboard_msgs::Load robotLoadSrv;

ros::ServiceClient robotPlay;
std_srvs::Trigger robotPlaySrv;

ros::ServiceClient robotPause;
std_srvs::Trigger robotPauseSrv;

ros::ServiceClient robotStop;
std_srvs::Trigger robotStopSrv;

ros::ServiceClient robotGetProgState;
ur_dashboard_msgs::GetProgramState robotGetProgStateSrv;

ros::ServiceClient urIoClient;
ur_digital_ports::digitalOut_srv gripper;

//Vision:
ros::ServiceClient visClient;
vision::check_brick visCmd;

//MES:
ros::ServiceClient mesGetClient;
mes_ordering::GetOrder_srv getOrder;

//OEE:
ros::Publisher oeePub;

//PackML:
ros::ServiceClient packmlGetClient;
system_manager::get_packml_state getPackml;

ros::Publisher packmlPub;

//Feeder:
bool getFeederStatus(system_manager::get_feeder_status::Request &req, system_manager::get_feeder_status::Response &res);

//Mutexes:
mutex feederLock;
mutex pauseLock;
mutex orderLock;
mutex stopLock;

//Functions:
void getOrderFromMes(int &orderId, int &b, int &r, int &y, string &ticket);
void pack(int color, int amount, int box);

int main(int argc, char** argv)
{
    ros::init(argc, argv, "system_manager");
    ros::NodeHandle n;

    ros::AsyncSpinner spinner(2);
    spinner.start();

    //Setup service clients:
    //Robot:
    robotLoadProgram = n.serviceClient<ur_dashboard_msgs::Load>("/ur_hardware_interface/dashboard/load_program");

    robotPlay = n.serviceClient<std_srvs::Trigger>("/ur_hardware_interface/dashboard/play");

    robotPause = n.serviceClient<std_srvs::Trigger>("/ur_hardware_interface/dashboard/pause");

    robotStop = n.serviceClient<std_srvs::Trigger>("/ur_hardware_interface/dashboard/stop");

    robotGetProgState = n.serviceClient<ur_dashboard_msgs::GetProgramState>("/ur_hardware_interface/dashboard/program_state");

    //MES:
    mesGetClient = n.serviceClient<mes_ordering::GetOrder_srv>("/MES_GetOrder");
    // mes_ordering::GetOrder_srv mesCmd;

    mes_ordering::GetOrder_srv getOrder;

    ros::ServiceClient mesDelClient = n.serviceClient<mes_ordering::DeleteOrder_srv>("/MES_DeleteOrder");

    mes_ordering::DeleteOrder_srv delOrder;

    //Vision:
    visClient = n.serviceClient<vision::check_brick>("/check_brick");

    //Publisher to PackML action-topic:
    packmlPub = n.advertise<std_msgs::Int8>("/action_state", 5);

    packmlGetClient = n.serviceClient<system_manager::get_packml_state>("/get_packml_state");

    //Publisher to feeder warning and alert:
    ros::Publisher feederWarningPub = n.advertise<std_msgs::Empty>("/feeder_warning", 1);
    ros::Publisher feederAlertPub = n.advertise<std_msgs::Empty>("/feeder_alert", 1);

    //Refill subscriber:
    ros::Subscriber feederRefillSub = n.subscribe("/feeder_refill", 1, feederRefill);

    //Gripper interface:
    //IO message for opening/closing gripper:
    gripper.request.port = 4;
    gripper.request.state = 1.0;

    urIoClient = n.serviceClient<ur_digital_ports::digitalOut_srv>("/digital_output");

    //MiR interface:
    ros::ServiceClient mirClient = n.serviceClient<mir_api::mir_api_action>("/mir_api/service");
    mir_api::mir_api_action mir;
    ros::Publisher mirPub = n.advertise<std_msgs::Int8>("/mir_api/sub", 10);

    //Sub. to system topics:
    ros::Subscriber pauseSub = n.subscribe<std_msgs::Empty>("/gui_pause", 1, pauseSystem);
    ros::Subscriber playSub = n.subscribe<std_msgs::Empty>("/gui_play", 1, playSystem);
    ros::Subscriber postSub = n.subscribe<std_msgs::Empty>("/gui_stop", 1, stopSystem);

    //OEE:
    oeePub = n.advertise<std_msgs::String>("/oee_calculator", 10);

    //Advertise feeder status service:
    ros::ServiceServer feederStatusService = n.advertiseService("/get_feeder_status", getFeederStatus);

    //Start packing:
    currentOrderIdx = 0;
    currentBox = 0;
    bricksDiscarded = 0;

    //Open the gripper:
    ROS_INFO("Opening the gripper");
    gripper.request.state = 0;
    if(!urIoClient.call(gripper))
    {
        ROS_ERROR("Failed to contact gripper");
        throw("Failed to contact gripper");
    }
    
    ROS_INFO("System manager started!");

    stopLock.lock();
    bool stopCopy = isStopped;
    stopLock.unlock();

    do
    {
        stopLock.lock();
        stopCopy = isStopped;
        stopLock.unlock();
        ros::Duration(0.1).sleep();
    } while(stopCopy);

    while(running)
    {
        //Stop-wait-loop:
        do
        {
            stopLock.lock();
            stopCopy = isStopped;
            stopLock.unlock();
            ros::Duration(0.1).sleep();
        } while(stopCopy);

        stopLock.lock();
        stopCopy = isStopped;
        stopLock.unlock();

        //Check feeder status:
        if(!stopCopy)
        {
            ROS_INFO("Checking feeder status..");
            std_msgs::Empty empty;

            feederLock.lock();
            if(feederEstimates[BLUE_BRICKS] <= 0 || feederEstimates[RED_BRICKS] <= 0 || feederEstimates[YELLOW_BRICKS] <= 0)
            {
                feederAlertPub.publish(empty);
                ROS_INFO("Waiting for feeders to be refilled.");
                
                //Go to "HOLDING":
                std_msgs::Int8 packmlAction;
                packmlAction.data = AC_HOLD;
                packmlPub.publish(packmlAction);

                //Go to "HELD":
                packmlAction.data = AC_SC;
                packmlPub.publish(packmlAction);

                //Wait for feeders to be refilled:
                int feederEstCopy[3];
                do
                {
                    feederLock.lock();
                    feederEstCopy[BLUE_BRICKS] = feederEstimates[BLUE_BRICKS];
                    feederEstCopy[RED_BRICKS] = feederEstimates[RED_BRICKS];
                    feederEstCopy[YELLOW_BRICKS] = feederEstimates[YELLOW_BRICKS];
                    feederLock.unlock();
                    ros::Duration(0.5).sleep();
                } while (feederEstimates[BLUE_BRICKS] != FEEDER_MAX || feederEstimates[RED_BRICKS] != FEEDER_MAX || feederEstimates[YELLOW_BRICKS] != FEEDER_MAX);

                //Go to "UNHOLDING":
                packmlAction.data = AC_UNHOLD;
                packmlPub.publish(packmlAction);

                //Go back to "EXECUTE":
                packmlAction.data = AC_SC;
                packmlPub.publish(packmlAction);

                
            }
            else if(feederEstimates[BLUE_BRICKS] <= FEEDER_WARNING_THRESH || feederEstimates[RED_BRICKS] <= FEEDER_WARNING_THRESH || feederEstimates[YELLOW_BRICKS] <= FEEDER_WARNING_THRESH)
            {
                feederWarningPub.publish(empty);
            }
            feederLock.unlock();
        }
        else
        {
            ROS_INFO("Stopped!");
        }


        stopLock.lock();
        stopCopy = isStopped;
        stopLock.unlock();

        if(!stopCopy)
        { 
            //Check if MiR is executing:
            mir.request.action = MIR_POLL_EXEC;
            if(!mirClient.call(mir))
            {
                ROS_ERROR("Service call to mir failed");
                throw("Service call to mir failed");
                return -1;
            }

            if(mir.response.result == 0) //MiR is not executing our mission
            { 
                //Go to "SUSPENDING":
                std_msgs::Int8 packmlAction;
                packmlAction.data = AC_SUSPEND;
                packmlPub.publish(packmlAction);

                //Go to "SUSPENDED":
                packmlAction.data = AC_SC;
                packmlPub.publish(packmlAction);

                ROS_INFO("Calling MiR");
                //Call MiR:
                std_msgs::Int8 mirMsg;
                mirMsg.data = MIR_CALL;
                mirPub.publish(MIR_CALL);

                do
                { 
                    //Check if MiR is executing:
                    mir.request.action = MIR_POLL_EXEC;
                    if(!mirClient.call(mir))
                    {
                        ROS_ERROR("Service call to mir failed");
                        throw("Service call to mir failed");
                        return -1;
                    }

                    ROS_INFO("Calling MiR");
                    //Call MiR:
                    std_msgs::Int8 mirMsg;
                    mirMsg.data = MIR_CALL;
                    mirPub.publish(MIR_CALL);

                    ros::Duration(2).sleep();
                    ROS_INFO("MiR is not executing!");
                } while(mir.response.result == 0);

                ROS_INFO("MiR is executing!");

                //Go to "UNSUSPENDING":
                packmlAction.data = AC_UNSUSPENDED;
                packmlPub.publish(packmlAction);

                //Go to "EXECUTE":
                packmlAction.data = AC_SC;
                packmlPub.publish(packmlAction);

                //     ROS_INFO("Calling MiR!");
                //     mir.request.action = MIR_CALL;
                //     if(!mirClient.call(mir))
                //     {
                //         ROS_ERROR("Service call to mir failed");
                //         throw("Service call to mir failed");
                //         return -1;
                //     }

                //     if(mir.response.result > 0)
                //     {
                //         ROS_INFO("MiR is on the way!");
                //     }
                //     else
                //     {
                //         ROS_INFO("MiR is busy");

                //         while (mir.response.result > 0)
                //         {
                //             ros::Duration(1).sleep();
                //             if (!mirClient.call(mir))
                //             {
                //                 ROS_ERROR("Service call to mir failed");
                //                 throw("Service call to mir failed");
                //                 return -1;
                //             }
                //         }
                //         ROS_INFO("MiR is on the way!");
                //     }
             }
        }

        stopLock.lock();
        stopCopy = isStopped;
        stopLock.unlock();

        if(!stopCopy)
        {
            ROS_INFO("Asking MES system for next order...");
            getOrderFromMes(currentOrderIds[currentOrderIdx], currentOrderContents[currentOrderIdx][BLUE_BRICKS], currentOrderContents[currentOrderIdx][RED_BRICKS], currentOrderContents[currentOrderIdx][YELLOW_BRICKS], currentOrderTickets[currentOrderIdx]);

            //Save original order contents:
            currentOrderContentsOrg[currentOrderIdx][BLUE_BRICKS] = currentOrderContents[currentOrderIdx][BLUE_BRICKS];
            currentOrderContentsOrg[currentOrderIdx][RED_BRICKS] = currentOrderContents[currentOrderIdx][RED_BRICKS];
            currentOrderContentsOrg[currentOrderIdx][YELLOW_BRICKS] = currentOrderContents[currentOrderIdx][YELLOW_BRICKS];

            ROS_INFO_STREAM("Got order no. " << currentOrderIds[currentOrderIdx]);
            ROS_INFO_STREAM("Ticket: " << currentOrderTickets[currentOrderIdx]);
            ROS_INFO_STREAM("Blue: " << currentOrderContents[currentOrderIdx][BLUE_BRICKS]);
            ROS_INFO_STREAM("Red: " << currentOrderContents[currentOrderIdx][RED_BRICKS]);
            ROS_INFO_STREAM("Yellow: " << currentOrderContents[currentOrderIdx][YELLOW_BRICKS]);

            ROS_INFO_STREAM("Using box no: " << currentBox);

            //OEE: Tell calc that I got new order:
            std_msgs::String s;
            s.data = "orderTaken";
            oeePub.publish(s);

            if(currentOrderTickets[currentOrderIdx] == "None")
            {
                std_msgs::Empty empty;
                feederAlertPub.publish(empty);
                ROS_INFO("Waiting for feeders to be refilled.");
                
                //Go to "HOLDING":
                std_msgs::Int8 packmlAction;
                packmlAction.data = AC_HOLD;
                packmlPub.publish(packmlAction);

                //Go to "HELD":
                packmlAction.data = AC_SC;
                packmlPub.publish(packmlAction);

                //Wait for feeders to be refilled:
                int feederEstCopy[3];
                do
                {
                    feederLock.lock();
                    feederEstCopy[BLUE_BRICKS] = feederEstimates[BLUE_BRICKS];
                    feederEstCopy[RED_BRICKS] = feederEstimates[RED_BRICKS];
                    feederEstCopy[YELLOW_BRICKS] = feederEstimates[YELLOW_BRICKS];
                    feederLock.unlock();
                    ros::Duration(0.5).sleep();
                } while (feederEstimates[BLUE_BRICKS] != FEEDER_MAX || feederEstimates[RED_BRICKS] != FEEDER_MAX || feederEstimates[YELLOW_BRICKS] != FEEDER_MAX);

                //Go to "UNHOLDING":
                packmlAction.data = AC_UNHOLD;
                packmlPub.publish(packmlAction);

                //Go back to "EXECUTE":
                packmlAction.data = AC_SC;
                packmlPub.publish(packmlAction);
            }
        }
        else
        {
            ROS_INFO("Stopped!");
        }

        if(currentOrderIdx == 0) //First order
        {
            firstOrderStartTime = ros::Time::now();
        }

        ROS_INFO_STREAM("Packing in box " << currentBoxString);

        stopLock.lock();
        stopCopy = isStopped;
        stopLock.unlock();

        if(!stopCopy)
        {
            ROS_INFO("Packing blue bricks");
            pack(BLUE_BRICKS, currentOrderContents[currentOrderIdx][BLUE_BRICKS], currentBox);
        }
        else
        {
            ROS_INFO("Stopped!");
        }

        stopLock.lock();
        stopCopy = isStopped;
        stopLock.unlock();

        if(!stopCopy)
        {
            ROS_INFO("Packing red bricks");
            pack(RED_BRICKS, currentOrderContents[currentOrderIdx][RED_BRICKS], currentBox);
        }
        else
        {
            ROS_INFO("Stopped!");
        }
        
        stopLock.lock();
        stopCopy = isStopped;
        stopLock.unlock();

        if(!stopCopy)
        {
            ROS_INFO("Packing yellow bricks");
            pack(YELLOW_BRICKS, currentOrderContents[currentOrderIdx][YELLOW_BRICKS], currentBox);

            if(currentOrderTickets[currentOrderIdx] != "None") //Don't pack/count non-orders
            {
                currentOrderIdx++;
                currentBox++;
                boxContainsOrder[currentBox] = true;
            }
        }
        else
        {
            ROS_INFO("Stopped!");
        }
        
        boxContainsOrder[currentBox] = true;

        ros::Duration timeSinceFirstOrder = ros::Time::now() - firstOrderStartTime;

        ROS_INFO_STREAM("Time elapsed since taking first order: " << timeSinceFirstOrder);

        if(timeSinceFirstOrder >= ros::Duration(600)) // 10 min.
        {
            ROS_ERROR("Too slow! Emptying all orders!");
            bricksDiscarded = 0;
            currentOrderIdx = 0;
            currentBox = 0;

            if(!loadAndRunUrProgram("rsd_discard_everything.urp"))
            {
                throw("Robot failed!");
                return -1;
            }
        }

        // stopLock.lock();
        // stopCopy = isStopped;
        // stopLock.unlock();

        // if(!stopCopy)
        // {
        //     if(currentOrderIdx == 3)
        //     {
                
        //     }
        // }

        stopLock.lock();
        stopCopy = isStopped;
        stopLock.unlock();

        if(!stopCopy)
        {
            if(currentOrderIdx == 4) //All 4 orders have been packed
            {
                //Go to "SUSPENDING":
                std_msgs::Int8 packmlAction;
                packmlAction.data = AC_SUSPEND;
                packmlPub.publish(packmlAction);

                //Go to "SUSPENDED":
                packmlAction.data = AC_SC;
                packmlPub.publish(packmlAction);

                //Wait for the MiR to arrive:
                mir.request.action = MIR_POLL_ARRIVED;
                if(!mirClient.call(mir))
                {
                    ROS_ERROR("Service call to mir failed");
                    throw("Service call to mir failed");
                    return -1;
                }

                stopLock.lock();
                stopCopy = isStopped;
                stopLock.unlock();

                if(stopCopy)
                    continue;

                while(mir.response.result == 0)
                {
                    ros::Duration(2).sleep();
                    if(!mirClient.call(mir))
                    {
                        ROS_ERROR("Service call to mir failed");
                        throw("Service call to mir failed");
                        return -1;
                    }
                }

                //When MiR has arrived:
                ROS_INFO("MiR has arrived!");

                //Go to "UNSUSPENDING":
                packmlAction.data = AC_UNSUSPENDED;
                packmlPub.publish(packmlAction);

                //Go to "EXECUTE":
                packmlAction.data = AC_SC;
                packmlPub.publish(packmlAction);

                //Move packed orders onto MiR:
                ROS_INFO("Placing order on MiR!");
                stopLock.lock();
                stopCopy = isStopped;
                stopLock.unlock();

                if(stopCopy)
                    continue;
                loadAndRunUrProgram("rsd_mir_place.urp");

                //Delete all orders:
                for(int i = 0; i < 4; i++)
                {
                    ROS_INFO_STREAM("Deleting order with id " << currentOrderIds[i]);
                    delOrder.request.id = currentOrderIds[i];
                    delOrder.request.ticket = currentOrderTickets[i];

                    if(!mesDelClient.call(delOrder))
                    {
                        ROS_INFO("Failed to delete order!");
                        return -1;
                    }

                }

                //Take empty boxes from MiR:
                ROS_INFO("Taking empty boxes from MiR");
                loadAndRunUrProgram("rsd_mir_pickup.urp");

                //Release the MiR:
                mir.request.action = MIR_RELEASE;
                if(!mirClient.call(mir))
                {
                    ROS_ERROR("Service call to mir failed");
                    throw("Service call to mir failed");
                    return -1;
                }
                ROS_INFO("MiR is released!");
                ros::Duration(2).sleep();

                //OEE:
                int totalBrickInOrders = 0;
                for(int i = 0; i < 4; i ++)
                    totalBrickInOrders += currentOrderContentsOrg[i][BLUE_BRICKS] + currentOrderContentsOrg[i][RED_BRICKS] + currentOrderContentsOrg[i][YELLOW_BRICKS];

                int totalBricksHandled = totalBrickInOrders + bricksDiscarded;
                
                std_msgs::String s;
                s.data = "done," + to_string(currentOrderIdx) + "," + to_string(totalBrickInOrders) + "," + to_string(totalBricksHandled);
                oeePub.publish(s);
                

                ROS_INFO("Deleted all four orders!");
                bricksDiscarded = 0;
                currentOrderIdx = 0;
                currentBox = 0;

            }
        }
    }

    return 0;
}

void feederRefill(const std_msgs::Empty::ConstPtr& msg)
{
    ROS_INFO("Getting feeder lock");
    feederLock.lock();
    feederEstimates[0] = FEEDER_MAX;
    feederEstimates[1] = FEEDER_MAX;
    feederEstimates[2] = FEEDER_MAX;
    feederLock.unlock();
    ROS_INFO("Feeder refilled!");
}

bool loadAndRunUrProgram(string filename)
{
    robotLoadSrv.request.filename = filename;

    if(!robotLoadProgram.call(robotLoadSrv))
    {
        ROS_ERROR_STREAM("Failed to load UR program " << robotLoadSrv.request.filename);
        return false;
    }

    //Check if system is paused:
    pauseLock.lock();
    bool isPausedCopy = isPaused;
    pauseLock.unlock();

    while(isPausedCopy)
    {
        pauseLock.lock();
        isPausedCopy = isPaused;
        pauseLock.unlock();
        ros::Duration(0.1).sleep();
    }

    if(!robotPlay.call(robotPlaySrv))
    {
        ROS_ERROR_STREAM("Failed to execute UR program " << robotLoadSrv.request.filename);
        throw("Failed to execute UR program!");
    }

    ros::Duration(1.0).sleep();

    do
    {
        if(!robotGetProgState.call(robotGetProgStateSrv))
        {
            ROS_ERROR("Failed to get robot program state!");
            return false;
        }

        ros::Duration(0.5).sleep();
    } while (robotGetProgStateSrv.response.state.state == robotGetProgStateSrv.response.state.PLAYING);
    
    return true;
}

void pauseSystem(const std_msgs::Empty::ConstPtr& msg)
{
    ROS_INFO("Pausing!");
    pauseLock.lock();
    if(isPaused)
    {
        pauseLock.unlock();
        return;
    }
    isPaused = true;
    pauseLock.unlock();

    std_msgs::String s;
    s.data = "downTime_start";
    oeePub.publish(s);
    ROS_INFO("Paused!");

    //Go to "HOLDING":
    std_msgs::Int8 packmlAction;
    packmlAction.data = AC_HOLD;
    packmlPub.publish(packmlAction);

    //Go to "HELD":
    packmlAction.data = AC_SC;
    packmlPub.publish(packmlAction);
}

void playSystem(const std_msgs::Empty::ConstPtr& msg)
{
    if(!packmlGetClient.call(getPackml))
    {
        ROS_ERROR("Failed to get PackML state via service");
        return;
    }

    feederLock.lock();
    if(feederEstimates[BLUE_BRICKS] <= 0 || feederEstimates[RED_BRICKS] <= 0 || feederEstimates[YELLOW_BRICKS] <= 0)
    {
        ROS_ERROR("Cannot play when feeders are empty!");
        feederLock.unlock();
        return;
    }
    feederLock.unlock();

    if(getPackml.response.state == ST_IDLE || getPackml.response.state == ST_HELD)
    {
        ROS_INFO("Play!");
        pauseLock.lock();
        if (isPaused)
        {
            ROS_INFO("Robot was paused!");
            isPaused = false;
            std_msgs::String s;
            s.data = "downTime_stop";
            oeePub.publish(s);
        }
        pauseLock.unlock();

        stopLock.lock();
        if (isStopped)
        {
            ROS_INFO("Robot was stopped!");
            isStopped = false;
            std_msgs::String s;
            s.data = "downTime_stop";
            oeePub.publish(s);
        }
        stopLock.unlock();

        //Publish PackML actions:
        std_msgs::Int8 packmlAction;
        if(getPackml.response.state == ST_IDLE)
        {
            packmlAction.data = AC_START;
            packmlPub.publish(packmlAction);
        }
        else
        {
            packmlAction.data = AC_UNHOLD;
            packmlPub.publish(packmlAction);
        }
        
        packmlAction.data = AC_SC;
        packmlPub.publish(packmlAction);  
    }
    else
    {
        ROS_INFO("Cannot press PLAY when not in idle state!");
    }
    
}

void stopSystem(const std_msgs::Empty::ConstPtr& msg)
{
    stopLock.lock();
    if(isStopped)
    {
         stopLock.unlock();
         return;
    }
    isStopped = true;
    stopLock.unlock();

    //Reset orders:
    orderLock.lock();
    for(int i = 0; i < 4; i++)
    {
        currentOrderContents[i][BLUE_BRICKS] = 0;
        currentOrderContents[i][RED_BRICKS] = 0;
        currentOrderContents[i][YELLOW_BRICKS] = 0;
        currentOrderIds[i] = -1;
        currentOrderTickets[i] = "";
    }
    orderLock.unlock();

    //OEE:
    std_msgs::String s;
    s.data = "STOP";
    oeePub.publish(s);

    //PackMl:
    std_msgs::Int8 i;
    i.data = 20; //AC_STOP
    packmlPub.publish(i);

    i.data = 0; //AC_SC
    packmlPub.publish(i);    
}

/**
 * Functions
 **/

void getOrderFromMes(int &orderId, int &b, int &r, int &y, string &ticket)
{
    orderLock.lock();
    //Call service:
    feederLock.lock();
    getOrder.request.blue = feederEstimates[BLUE_BRICKS];
    getOrder.request.red = feederEstimates[RED_BRICKS];
    getOrder.request.yellow = feederEstimates[YELLOW_BRICKS];
    feederLock.unlock();

    if(!mesGetClient.call(getOrder))
    {
        ROS_ERROR("MES servicer call failed!");
        orderLock.unlock();
        throw("MES servicer call failed!");
        return;
    }

    //Update param.:
    orderId = getOrder.response.id;
    b = getOrder.response.blue;
    r = getOrder.response.red;
    y = getOrder.response.yellow;
    ticket = getOrder.response.ticket;
    orderLock.unlock();
}

void pack(int color, int amount, int box)
{
    ROS_INFO("Packing order!");
    ROS_INFO_STREAM("Color: " << color);
    ROS_INFO_STREAM("Amount: " << amount);
    ROS_INFO_STREAM("Box: " << box);

    string urPickProg;
    string colorName;

    switch(color)
    {
        case BLUE_BRICKS:
            urPickProg = "rsd_pick_small.urp";
            colorName = "blue";
            break;
        case RED_BRICKS:
            urPickProg = "rsd_pick_medium.urp";
            colorName = "red";
            break;
        case YELLOW_BRICKS:
            urPickProg = "rsd_pick_big.urp";
            colorName = "yellow";
            break;
    }

    bool stopCopy;
    while(currentOrderContents[currentOrderIdx][color] > 0) //Pack all the "color" bricks
    {
        //TODO::Try max 3 times!

        //Pickup a brick:
        if(!loadAndRunUrProgram(urPickProg))
        {
            ROS_ERROR("Robot failed!");
            throw("Robot failed!");
            return;
        }

        //We picked a brick:
        feederEstimates[color]--;

        stopLock.lock();
        stopCopy = isStopped;
        stopLock.unlock();

        if(stopCopy)
            return;

        //Ask vision system to validate brick:
        ros::WallTime start_ = ros::WallTime::now();
        visCmd.request.color = colorName;
        if(!visClient.call(visCmd))
        {
            ROS_ERROR("Vision service call failed!");
            throw("Vision service call failed!");
            return;
        }
        ros::WallTime end_ = ros::WallTime::now();
        double execution_time = (end_ - start_).toNSec() * 1e-6;
        ROS_INFO_STREAM("Vision time [ms]: " << execution_time);

        ROS_INFO_STREAM("Brick check result: " << (int)visCmd.response.result);
        if(visCmd.response.result == BRICK_MATCH)
        {
            //Ask robot to go to current box:
            string boxProgram;
            switch (box)
            {
            case 0:
                boxProgram = "rsd_box_a.urp";
                break;
            case 1:
                boxProgram = "rsd_box_b.urp";
                break;
            case 2:
                boxProgram = "rsd_box_c.urp";
                break;
            case 3:
                boxProgram = "rsd_box_d.urp";
                break;
            
            default:
                ROS_ERROR("Invalid box was selected. Program error, please debug.");
                throw("Invalid box was selected. Program error, please debug");
                return;
            }

            ROS_INFO("**Waiting for stop lock**");
            stopLock.lock();
            stopCopy = isStopped;
            stopLock.unlock();

            if(stopCopy)
                return;
            
            if(!loadAndRunUrProgram(boxProgram))
            {
                ROS_ERROR("Robot failed!");
                throw("Robot failed!");
                return;
            }

            //Ask vision system to validate brick:
            if(!visClient.call(visCmd))
            {
                ROS_ERROR("Vision service call failed!");
                throw("Vision service call failed!");
                return;
            }

            if(visCmd.response.result == BRICK_MATCH)
            {
                currentOrderContents[currentOrderIdx][color]--; //One less brick to pack :)
                //Open the gripper:
                ROS_INFO("Opening the gripper");
                gripper.request.state = 0.0;
                if(!urIoClient.call(gripper))
                {
                    ROS_ERROR("Failed to contact gripper");
                    throw("Failed to contact gripper");
                    return;
                }

                stopLock.lock();
                stopCopy = isStopped;
                stopLock.unlock();

                if(stopCopy)
                    return;      
            }
            else
            {
                ROS_ERROR("No brick to drop!");

                stopLock.lock();
                stopCopy = isStopped;
                stopLock.unlock();

                if(stopCopy)
                    return;
                //Ask robot to go to aboveDiscard
                if(!loadAndRunUrProgram("rsd_discard.urp"))
                {
                    ROS_ERROR("Robot failed!");
                    throw("Robot failed!");
                    return;
                }
                bricksDiscarded++;
            }
        }
        else
        {
            stopLock.lock();
            stopCopy = isStopped;
            stopLock.unlock();

            if(stopCopy)
                return;
            //Ask robot to go to aboveDiscard
            if(!loadAndRunUrProgram("rsd_discard.urp"))
            {
                ROS_ERROR("Robot failed!");
                throw("Robot failed!");
                return;
            }

            //Open the gripper:
            ROS_INFO("Opening the gripper");
            gripper.request.state = 0.0;
            if(!urIoClient.call(gripper))
            {
                ROS_ERROR("Failed to contact gripper");
                throw("Failed to contact gripper");
            }
            bricksDiscarded++;
        }
    }
}

bool getFeederStatus(system_manager::get_feeder_status::Request &req, system_manager::get_feeder_status::Response &res)
{
    feederLock.lock();
    res.blue = feederEstimates[BLUE_BRICKS];
    res.red = feederEstimates[RED_BRICKS];
    res.yellow = feederEstimates[YELLOW_BRICKS];
    feederLock.unlock();

    return true;
}