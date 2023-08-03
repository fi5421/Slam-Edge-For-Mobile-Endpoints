/**
 * This file is part of ORB-SLAM2.
 *
 * Copyright (C) 2014-2016 Raúl Mur-Artal <raulmur at unizar dot es> (University of Zaragoza)
 * For more information see <https://github.com/raulmur/ORB_SLAM2>
 *
 * ORB-SLAM2 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ORB-SLAM2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ORB-SLAM2. If not, see <http://www.gnu.org/licenses/>.
 */

#include "LocalMapping.h"
#include "LoopClosing.h"
#include "ORBmatcher.h"
#include "Optimizer.h"

#include "vector"

#include <mutex>

class my_exception : public std::runtime_error
{
    std::string msg;

public:
    my_exception(const std::string &arg, const char *file, int line) : std::runtime_error(arg)
    {
        std::ostringstream o;
        o << file << ":" << line << ": " << arg;
        msg = o.str();
    }
    ~my_exception() throw() {}
    const char *what() const throw()
    {
        return msg.c_str();
    }
};

namespace ORB_SLAM2
{
    // Edge-SLAM: measure
    std::chrono::high_resolution_clock::time_point LocalMapping::msLastMUStart = std::chrono::high_resolution_clock::now();
    std::chrono::high_resolution_clock::time_point LocalMapping::msLastMUStop;

    // Edge-SLAM: map update variables
    const int LocalMapping::MAP_FREQ = 5000;
    const int LocalMapping::KF_NUM = 6;
    const int LocalMapping::CONN_KF = 2;
    bool LocalMapping::msNewKFFlag = false;
    stack<long unsigned int> LocalMapping::msLatestKFsId;

    // Edge-SLAM: relocalization
    vector<KeyFrame *> LocalMapping::vpCandidateKFs;
    unordered_set<long unsigned int> LocalMapping::usCandidateKFsId;
    bool LocalMapping::msRelocStatus = false;
    bool LocalMapping::msRelocNewFFlag = false;
    const int LocalMapping::RELOC_FREQ = 5000;

    // Edge-SLAM: added settings file path variable
    LocalMapping::LocalMapping(Map *pMap, KeyFrameDatabase *pKFDB, ORBVocabulary *pVoc, const string &strSettingPath, const float bMonocular, int edgeNumber_p) : mbMonocular(bMonocular), mbResetRequested(false), mbFinishRequested(false), mbFinished(true), mpORBVocabulary(pVoc), mpMap(pMap), mpKeyFrameDB(pKFDB),
                                                                                                                                                                  mbAbortBA(false), mbStopped(false), mbStopRequested(false), mbNotStop(false), mbAcceptKeyFrames(true)
    {
        // Edge-SLAM: everything in this scope is new

        // Load camera parameters from settings file
        cout << "Edge Number " << edgeNumber_p << endl;
        cv::FileStorage fSettings(strSettingPath, cv::FileStorage::READ);
        float fps = fSettings["Camera.fps"];
        if (fps == 0)
            fps = 30;
        // Max/Min Frames to insert keyframes and to check relocalisation
        mMinFrames = 0;
        mMaxFrames = fps;

        edgeNumber = edgeNumber_p;
        if (edgeNumber == 1)
        {
            activeEdge = true;
        }

        // Setting up connections
        string ip;
        string port_number;
        int port_int;
        string subset_port;
        cout << "Enter the device IP address: ";
        // getline(cin, ip);
        ip = "127.0.0.1";

        // Subset Tcp Connection
        cout << "Enter subset Port Number\n";
        getline(cin, subset_port);
        cout << "Subset Port " << std::stoi(subset_port) << endl;
        if (edgeNumber == 1)
        {
            map_subset_socket = new TcpSocket(ip, std::stoi(subset_port));
            map_subset_socket->waitForConnection();
            subset_thread = new thread(&ORB_SLAM2::LocalMapping::tcp_send, &map_subset_queue_send, map_subset_socket, "subset map");
        }
        else
        {
            map_subset_socket = new TcpSocket(ip, std::stoi(subset_port), ip, std::stoi(subset_port));

            map_subset_socket->sendConnectionRequest();
            subset_thread = new thread(&ORB_SLAM2::LocalMapping::tcp_receive, &keyframe_queue, map_subset_socket, 1, "subset map");
        }

        // map_subset_queue_send.enqueue("Temp Message");

        // Keyframe connection
        cout << "Enter the port number used for keyframe connection: ";
        getline(cin, port_number);
        port_int = std::stoi(port_number);

        keyframe_socket = new TcpSocket(ip, std::stoi(port_number));
        keyframe_socket->waitForConnection();
        keyframe_thread = new thread(&ORB_SLAM2::LocalMapping::tcp_receive, &keyframe_queue, keyframe_socket, 2, "keyframe");
        // Frame connection
        // cout << "Enter the port number used for frame connection: ";
        // getline(cin, port_number);
        port_int += 2;
        cout << "Port number for Frame Connection: " << port_int << endl;
        frame_socket = new TcpSocket(ip, port_int);
        frame_socket->waitForConnection();
        frame_thread = new thread(&ORB_SLAM2::LocalMapping::tcp_receive, &frame_queue, frame_socket, 1, "frame");
        // Map connection
        // cout << "Enter the port number used for map connection: ";
        // getline(cin, port_number);
        port_int += 2;
        cout << "Port Number for map connection " << port_int << endl;
        map_socket = new TcpSocket(ip, port_int);
        map_socket->waitForConnection();
        map_thread = new thread(&ORB_SLAM2::LocalMapping::tcp_send, &map_queue, map_socket, "map");

        mnLastKeyFrameId = 0;

        cout << "log,LocalMapping::LocalMapping,done" << endl;
    }

    // Edge-SLAM
    void LocalMapping::keyframeCallback(const std::string &msg)
    {
        // If Local Mapping is freezed by a Loop Closure do not insert keyframes
        if (isStopped() || stopRequested())
            return;

        // If system reset is in process, then return
        if (CheckReset())
            return;

        // Moved from CreateNewKeyFrame() function in tracking thread on client
        if (!SetNotStop(true))
            return;

        KeyFrame *tKF = new KeyFrame();

        // Making the Key Frame object
        try
        {
            std::stringstream iis(msg);             // declaring msg as a stream
            boost::archive::text_iarchive iia(iis); // used for serializing
            iia >> tKF;                             // making a key frame object from the serialized boost object
        }
        catch (boost::archive::archive_exception e)
        {
            cout << "log,LocalMapping::keyframeCallback,error: " << e.what() << endl;
            SetNotStop(false);
            return;
        }

        // Check for reset signal from client
        // reset caught on server
        if (edgeNumber == 2)
        {
            cout << "log,LocalMapping::KeyframeCallback, called on KeyFrame: " << tKF->mnId << endl;
        }
        if (tKF->GetResetKF() || (tKF->mnFrameId < mnLastKeyFrameId))
        {
            cout << "log,LocalMapping::keyframeCallback,received reset signal from client with keyframe " << tKF->mnId << endl;

            RequestReset();
            SetNotStop(false);
            return;
        }

        tKF->setORBVocab(mpORBVocabulary);
        tKF->setMapPointer(mpMap);
        tKF->setKeyFrameDatabase(mpKeyFrameDB);

        // Check keyframe after receiving it from the client
        // If first received keyframe then insert without additional checking
        if ((tKF->mnId < 1) || (mpMap->KeyFramesInMap() < 1))
        {
            tKF->ChangeParent(NULL);
            InsertKeyFrame(tKF);
            mpMap->mvpKeyFrameOrigins.push_back(tKF);
            msLatestKFsId.push(tKF->mnId);
            mnLastKeyFrameId = tKF->mnFrameId;

            msNewKFFlag = true;

            cout << "log,LocalMapping::keyframeCallback,accepted initial keyframe " << tKF->mnId << endl;
        }
        else
        {
            if (NeedNewKeyFrame(tKF))
            {
                InsertKeyFrame(tKF);
                msLatestKFsId.push(tKF->mnId);
                mnLastKeyFrameId = tKF->mnFrameId;

                msNewKFFlag = true;

                cout << "log,LocalMapping::keyframeCallback,accepted keyframe " << tKF->mnId << endl;
            }
            else
                cout << "log,LocalMapping::keyframeCallback,dropped keyframe " << tKF->mnId << endl;
        }

        // Set relocalization variables
        msRelocStatus = false;
        msRelocNewFFlag = false;

        SetNotStop(false);

        cout << "log,LocalMapping::keyframeCallback, returning\n";
    }

    void LocalMapping::SetLoopCloser(LoopClosing *pLoopCloser)
    {
        mpLoopCloser = pLoopCloser;
    }

    // Edge-SLAM: disabled
    /*
    void LocalMapping::SetTracker(Tracking *pTracker)
    {
        mpTracker=pTracker;
    }
    */

    // Edge-SLAM
    // multi edge
    void LocalMapping::Reset()
    {
        /* Edge-SLAM: disabled because viewer is disabled on server
        cout << "System Reseting" << endl;
        if(mpViewer)
        {
            mpViewer->RequestStop();
            while(!mpViewer->isStopped())
                usleep(3000);
        }*/

        // Reset Loop Closing
        cout << "Reseting Loop Closing...";
        mpLoopCloser->RequestReset();
        cout << " done" << endl;

        // Clear BoW Database
        cout << "Reseting Database...";
        mpKeyFrameDB->clear();
        cout << " done" << endl;

        // Clear Map (this erase MapPoints and KeyFrames)
        mpMap->clear();

        KeyFrame::nNextId = 0;
        Frame::nNextId = 0;

        /* Edge-SLAM: disabled because viewer is disabled on server
        if(mpViewer)
            mpViewer->Release();*/
    }

    void LocalMapping::ProcessSubset(std::string msg){
        vector<std::string> mapVec;
         try
        {
            std::stringstream is(msg);
            boost::archive::text_iarchive ia(is);
            ia >> mapVec;
            is.clear();
        }
        catch (boost::archive::archive_exception e)
        {
            cout << "log,LocalMapping::ProcessSubset,map error: " << e.what() << endl;
            return;
        }

        for (int i =0;i<(int)mapVec.size();i++){
            KeyFrame *tKF = new KeyFrame();
            {
                try{
                     std::stringstream iis(mapVec[i]);
                    boost::archive::text_iarchive iia(iis);
                    iia >> tKF;
                    iis.clear();
                }catch (boost::archive::archive_exception e)
                {
                    cout<<"log, LocalMapping::ProcessSubset, KF error"<<e.what()<<endl;
                    return;
                }

            }

            tKF->setORBVocab(mpORBVocabulary);
            tKF->setMapPointer(mpMap);
            tKF->setKeyFrameDatabase(mpKeyFrameDB);
            tKF->ComputeBoW();

            vector<MapPoint *> vpMapPointMatches = tKF->GetMapPointMatches();

             for (size_t i = 0; i < vpMapPointMatches.size(); i++)
            {
                MapPoint *pMP = vpMapPointMatches[i];
                if (pMP)
                {
                    if (!pMP->isBad())
                    {
                        // If tracking id is set
                        if (pMP->trSet)
                        {
                            MapPoint *pMPMap = mpMap->RetrieveMapPoint(pMP->mnId, true);

                            if (pMPMap != NULL)
                            {
                                // Replace keyframe's mappoint pointer to the existing one in tracking local-map
                                tKF->AddMapPoint(pMPMap, i);

                                // Add keyframe observation to the mappoint
                                pMPMap->AddObservation(tKF, i);

                                // Delete duplicate mappoint
                                delete pMP;
                            }
                            else
                            {
                                // Add keyframe's mappoint to tracking local-map
                                mpMap->AddMapPoint(pMP);

                                // Add keyframe observation to the mappoint
                                pMP->AddObservation(tKF, i);
                                pMP->setMapPointer(mpMap); // We are not sending the map pointer in marshalling
                                pMP->SetReferenceKeyFrame(tKF);
                            }
                        }
                        else if (pMP->lmSet) // If tracking id is not set, but local-mapping id is set
                        {
                            MapPoint *pMPMap = mpMap->RetrieveMapPoint(pMP->lmMnId, false);

                            if (pMPMap != NULL)
                            {
                                // Replace keyframe's mappoint pointer to the existing one in tracking local-map
                                tKF->AddMapPoint(pMPMap, i);

                                // Add keyframe observation to the mappoint
                                pMPMap->AddObservation(tKF, i);

                                // Delete duplicate mappoint
                                delete pMP;
                            }
                            else
                            {
                                // Assign tracking id
                                pMP->AssignId(true);

                                // Add keyframe's mappoint to tracking local-map
                                mpMap->AddMapPoint(pMP);

                                // Add keyframe observation to the mappoint
                                pMP->AddObservation(tKF, i);
                                pMP->setMapPointer(mpMap); // We are not sending the map pointer in marshalling
                                pMP->SetReferenceKeyFrame(tKF);
                            }
                        }
                    }
                }
            }

            mpMap->AddKeyFrame(tKF);

            mpKeyFrameDB->add(tKF);

            tKF=static_cast<KeyFrame *>(NULL);
            vpMapPointMatches.clear();
       }

         vector<KeyFrame *> vpKeyFrames = mpMap->GetAllKeyFrames();

        // Initialize Reference KeyFrame and other KF variables
        if (vpKeyFrames.size() > 0)
        {
            if (!refKFSet)
                mpReferenceKF = vpKeyFrames[0];
            else
            {
                if (mpReferenceKF->mnId < vpKeyFrames[0]->mnId)
                {
                    mpReferenceKF = vpKeyFrames[0];
                    refKFSet = false;
                }
            }

            mnLastKeyFrameId = vpKeyFrames[0]->mnFrameId;
            mpLastKeyFrame = vpKeyFrames[0];
            mnMapUpdateLastKFId = vpKeyFrames[0]->mnId;
        }

        for (std::vector<KeyFrame *>::iterator it = vpKeyFrames.begin(); it != vpKeyFrames.end(); ++it)
        {
            KeyFrame *pKFCon = *it;

            pKFCon->ReconstructConnections();

            // Edge-SLAM: debug
            cout << pKFCon->mnId << " ";

            // If RefKF has lower id than current KF, then set it to that KF
            if (mpReferenceKF->mnId < pKFCon->mnId)
            {
                mpReferenceKF = pKFCon;
                refKFSet = false;
            }

            // Update other KF variables
            if (mnMapUpdateLastKFId < pKFCon->mnId)
            {
                mnLastKeyFrameId = pKFCon->mnFrameId;
                mpLastKeyFrame = pKFCon;
                mnMapUpdateLastKFId = pKFCon->mnId;
            }
        }

         mCurrentFrame.mpReferenceKF = mpReferenceKF;

        // Get all map points in tracking local-map
        vector<MapPoint *> vpMapPoints = mpMap->GetAllMapPoints();

          for (std::vector<MapPoint *>::iterator it = vpMapPoints.begin(); it != vpMapPoints.end(); ++it)
        {
            MapPoint *rMP = *it;

            if ((unsigned)rMP->mnFirstKFid == rMP->GetReferenceKeyFrame()->mnId)
                continue;

            KeyFrame *rKF = mpMap->RetrieveKeyFrame(rMP->mnFirstKFid);

            if (rKF)
                rMP->SetReferenceKeyFrame(rKF);
        }

         for (int i = 0; i < mLastFrame.N; i++)
        {
            if (lastFrame_points_availability[i])
            {
                MapPoint *newpMP = mpMap->RetrieveMapPoint(lastFrame_points_ids[i], true);

                if (newpMP)
                {
                    mLastFrame.mvpMapPoints[i] = newpMP;
                }
                else
                {
                    mLastFrame.mvpMapPoints[i] = static_cast<MapPoint *>(NULL);
                }
            }
        }

        // We should update mvpLocalMapPoints for viewer
        mvpLocalMapPoints.clear();
        for (unsigned int i = 0; i < mvpLocalMapPoints_ids.size(); i++)
        {
            MapPoint *pMP = mpMap->RetrieveMapPoint(mvpLocalMapPoints_ids[i], true);
            if (pMP)
            {
                mvpLocalMapPoints.push_back(pMP);
            }
        }

    }

    void LocalMapping::Run()
    {
        mbFinished = false;

        while (1)
        {
            // Edge-SLAM: check if there is a new keyframe received, if not, then check if a frame is received for relocalization
            {
                string msg;
                if (keyframe_queue.try_dequeue(msg))
                {
                    // If relocalization was successful and a new keyframe is received, then drop any remaining relocalization frames in queue
                    if (msg.length() < 100)
                    {
                        cout << "Message received on KeyFrame Socket " << msg << endl;
                        if (msg == "Start Sync" && edgeNumber == 2)
                        {
                            sync = true;
                        }
                        else if (msg == "End Sync" && edgeNumber == 2)
                        {
                            sync = false;
                        }
                        else if (msg == "Active Edge")
                        {
                            activeEdge = true;
                        }
                    }
                    else
                    {
                        if(sync){
                            ProcessSubset(msg);
                        }
                        if (msRelocStatus)
                        {
                            string data;
                            if (frame_queue.try_dequeue(data))
                            {
                                data.clear();
                            }
                        }
                        // inserts into  mlNewKeyFrames
                        keyframeCallback(msg);
                        cout << "log,LocalMapping::Run, coming out of keyframeCallback\n";
                    }
                }
                else if (frame_queue.try_dequeue(msg))
                {
                    if (msg.length() < 100)
                    {
                        cout << "Message on Frame Socket " << msg << endl;
                        if (msg == "Start Sync")
                        {
                            startSync();
                        }
                    }
                    else
                    {
                        frameCallback(msg);
                    }
                }
            }

            // Tracking will see that Local Mapping is busy
            SetAcceptKeyFrames(false);

            // Check if there are keyframes in the queue
            if (CheckNewKeyFrames())
            {
                cout << "log,LocalMapping::Run, in first if\n";
                // BoW conversion and insertion in Map
                ProcessNewKeyFrame();
                cout << "log,LocalMapping::Run, out of processnewkeyframe\n";

                // Edge-SLAM: check if new keyframe is received
                {
                    string msg;
                    if (keyframe_queue.try_dequeue(msg))
                        keyframeCallback(msg);
                }
                // cout<<"log,LocalMapping::Run, 1\n";
                // Check recent MapPoints
                MapPointCulling();
                // cout<<"log,LocalMapping::Run, 2\n";

                // Triangulate new MapPoints
                CreateNewMapPoints();
                // cout<<"log,LocalMapping::Run, 3\n";

                if (!CheckNewKeyFrames())
                {
                    if (edgeNumber == 2)
                    {
                        cout << "log,LocalMapping::Run, 3.1\n";
                    }

                    // Find more matches in neighbor keyframes and fuse point duplications
                    try
                    {
                        SearchInNeighbors();
                    }
                    catch (const std::runtime_error &ex)
                    {
                        cout << ex.what() << endl;
                    }
                    // cout<<"log,LocalMapping::Run, 3.2\n";
                }
                if (edgeNumber == 2)
                {
                    cout << "log,LocalMapping::Run, 4\n";
                }

                mbAbortBA = false;

                if (!CheckNewKeyFrames() && !stopRequested())
                {
                    // Local BA
                    if (edgeNumber == 2)
                    {
                        cout << "log,LocalMapping::Run, 4.1\n";
                    }
                    if (mpMap->KeyFramesInMap() > 2)
                    {
                        if (edgeNumber == 2)
                        {
                            cout << "log,LocalMapping::Run, 4.2\n";
                        }
                        if (activeEdge)
                        {
                            Optimizer::LocalBundleAdjustment(mpCurrentKeyFrame, &mbAbortBA, mpMap);
                        }
                    }
                    if (edgeNumber == 2)
                    {
                        cout << "log,LocalMapping::Run, 4.3\n";
                    }
                    // Check redundant local Keyframes
                    KeyFrameCulling();
                    if (edgeNumber == 2)
                    {
                        cout << "log,LocalMapping::Run, 4.4\n";
                    }
                }
                if (edgeNumber == 2)
                {
                    cout << "log,LocalMapping::Run, 5\n";
                }

                mpLoopCloser->InsertKeyFrame(mpCurrentKeyFrame);
                // cout<<"log,LocalMapping::Run, 1\n";

                // Edge-SLAM: debug
                cout << "log,LocalMapping::Run,map has " << mpMap->MapPointsInMap() << " mappoints and " << mpMap->KeyFramesInMap() << " keyframes" << endl;
            }
            else if (Stop())
            {
                // Safe area to stop
                while (isStopped() && !CheckFinish())
                {
                    usleep(3000);
                }
                if (CheckFinish())
                {
                    cout << "here1\n";
                    break;
                }
            }

            // Edge-SLAM: create and send local-map update
            // Edge-SLAM: added curly brackets (block) to limit measurement variables scope
            {
                // Edge-SLAM: measure
                // Edge-SLAM: check how long it has been since last map update
                msLastMUStop = std::chrono::high_resolution_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(msLastMUStop - msLastMUStart);
                auto dCount = duration.count();

                // Send relocalization map
                if (activeEdge && (dCount > RELOC_FREQ) && (mpMap->KeyFramesInMap() > 0) && (msRelocStatus) && (msRelocNewFFlag) && (!CheckReset()))
                {
                    sendRelocMapUpdate();
                }
                // Send regular local map update
                else if (activeEdge && (dCount > MAP_FREQ) && (mpMap->KeyFramesInMap() > 0) && (msNewKFFlag) && (!CheckReset()))
                {
                    sendLocalMapUpdate();
                }
            }

            ResetIfRequested();

            // Tracking will see that Local Mapping is free
            SetAcceptKeyFrames(true);
            // mbFinishRequested
            if (CheckFinish())
            {
                cout << "here2\n";
                break;
            }

            usleep(3000);
        }
        cout << "here out of while loop, local mapping\n";
        SetFinish();
    }

    void LocalMapping::InsertKeyFrame(KeyFrame *pKF)
    {
        unique_lock<mutex> lock(mMutexNewKFs);
        mlNewKeyFrames.push_back(pKF);
        mbAbortBA = true;

        // Edge-SLAM: debug
        cout << "log,LocalMapping::InsertKeyFrame,queue size " << mlNewKeyFrames.size() << endl;
    }

    bool LocalMapping::CheckNewKeyFrames()
    {
        unique_lock<mutex> lock(mMutexNewKFs);
        return (!mlNewKeyFrames.empty());
    }

    void LocalMapping::ProcessNewKeyFrame()
    {
        {
            unique_lock<mutex> lock(mMutexNewKFs);
            mpCurrentKeyFrame = mlNewKeyFrames.front();
            mlNewKeyFrames.pop_front();
        }

        if (edgeNumber == 2)
        {
            cout << "LocalMapping::ProcessNewKeyFrame, 1\n";
        }

        // Compute Bags of Words structures
        mpCurrentKeyFrame->ComputeBoW();

        if (edgeNumber == 2)
        {
            cout << "LocalMapping::ProcessNewKeyFrame, 1\n";
        }

        // Associate MapPoints to the new keyframe and update normal and descriptor
        const vector<MapPoint *> vpMapPointMatches = mpCurrentKeyFrame->GetMapPointMatches();

        if (edgeNumber == 2)
        {
            cout << "LocalMapping::ProcessNewKeyFrame, 2\n";
        }
        for (size_t i = 0; i < vpMapPointMatches.size(); i++)
        {
            MapPoint *pMP = vpMapPointMatches[i];
            if (pMP)
            {
                if (!pMP->isBad())
                {
                    // Edge-SLAM: since we are not sending mObservations and pRefKF due to being pointers, we should take care of their adjustment

                    // Edge-SLAM: set mappoint refKF
                    if ((unsigned)pMP->mnFirstKFid != mpCurrentKeyFrame->mnId)
                    {
                        KeyFrame *pRefKF = mpMap->RetrieveKeyFrame(pMP->mnFirstKFid);
                        if (pRefKF)
                        {
                            pMP->SetReferenceKeyFrame(pRefKF);
                        }
                        else
                        {
                            pMP->SetReferenceKeyFrame(static_cast<KeyFrame *>(NULL));
                        }
                    }
                    else
                    {
                        pMP->SetReferenceKeyFrame(mpCurrentKeyFrame);
                    }

                    // Edge-SLAM
                    // This if condition corresponds to map points that have been sent previously through previous keyframes
                    if (!pMP->IsInKeyFrame(mpCurrentKeyFrame->mnId)) // Changing the function to receive id instead of keyframe pointer
                    {
                        if (pMP->lmSet) // This shows the local mapping id of mappoint is set
                        {
                            pMP = mpMap->RetrieveMapPoint(pMP->lmMnId, false);
                        }
                        else // If local mapping id is not set, we try to find the mappoint by tracking id
                        {
                            pMP = mpMap->RetrieveMapPoint(pMP->mnId, true);
                        }

                        if (pMP) // If it has not been removed and its available in the global map
                        {
                            pMP->AddObservation(mpCurrentKeyFrame, i);
                            pMP->UpdateNormalAndDepth();

                            pMP->ComputeDistinctiveDescriptors();

                            // Add the corresponding address in global map
                            mpCurrentKeyFrame->AddMapPoint(pMP, i);
                        }
                        else
                        {
                            mpCurrentKeyFrame->EraseMapPointMatch(i);
                        }
                    }
                    else // this can only happen for new stereo points inserted by the Tracking
                    {
                        pMP->AssignId(false);
                        pMP->AddObservation(mpCurrentKeyFrame, i); // Because we haven't transferred mObservations, we need it

                        mlpRecentAddedMapPoints.push_back(pMP);

                        mpMap->AddMapPoint(pMP);   // Since we have a separate map from tracking map
                        pMP->setMapPointer(mpMap); // We are not sending the map pointer in marshalling
                    }
                }
            }
        }

        if (edgeNumber == 2)
        {
            cout << "LocalMapping::ProcessNewKeyFrame, 3\n";
        }

        // Update links in the Covisibility Graph
        if (edgeNumber == 2)
        {
            mpCurrentKeyFrame->UpdateConnections(true);
        }
        else
        {
            mpCurrentKeyFrame->UpdateConnections(false);
        }
    

    if(edgeNumber == 2)
    {
        cout << "LocalMapping::ProcessNewKeyFrame, 4\n";
    }

    // Insert Keyframe in Map
    mpMap->AddKeyFrame(mpCurrentKeyFrame);

    if (edgeNumber == 2)
    {
        cout << "LocalMapping::ProcessNewKeyFrame, 5\n";
    }
}

void LocalMapping::MapPointCulling()
{
    // Check Recent Added MapPoints
    list<MapPoint *>::iterator lit = mlpRecentAddedMapPoints.begin();
    const unsigned long int nCurrentKFid = mpCurrentKeyFrame->mnId;

    int nThObs;
    if (mbMonocular)
        nThObs = 2;
    else
        nThObs = 3;
    const int cnThObs = nThObs;

    while (lit != mlpRecentAddedMapPoints.end())
    {
        MapPoint *pMP = *lit;
        if (pMP->isBad())
        {
            lit = mlpRecentAddedMapPoints.erase(lit);
        }
        else if (pMP->GetFoundRatio() < 0.25f)
        {
            pMP->SetBadFlag();
            lit = mlpRecentAddedMapPoints.erase(lit);
        }
        else if (((int)nCurrentKFid - (int)pMP->mnFirstKFid) >= 2 && pMP->Observations() <= cnThObs)
        {
            pMP->SetBadFlag();
            lit = mlpRecentAddedMapPoints.erase(lit);
        }
        else if (((int)nCurrentKFid - (int)pMP->mnFirstKFid) >= 3)
            lit = mlpRecentAddedMapPoints.erase(lit);
        else
            lit++;
    }
}

void LocalMapping::CreateNewMapPoints()
{
    // Retrieve neighbor keyframes in covisibility graph
    int nn = 10;
    if (mbMonocular)
        nn = 20;
    const vector<KeyFrame *> vpNeighKFs = mpCurrentKeyFrame->GetBestCovisibilityKeyFrames(nn);

    ORBmatcher matcher(0.6, false);

    cv::Mat Rcw1 = mpCurrentKeyFrame->GetRotation();
    cv::Mat Rwc1 = Rcw1.t();
    cv::Mat tcw1 = mpCurrentKeyFrame->GetTranslation();
    cv::Mat Tcw1(3, 4, CV_32F);
    Rcw1.copyTo(Tcw1.colRange(0, 3));
    tcw1.copyTo(Tcw1.col(3));
    cv::Mat Ow1 = mpCurrentKeyFrame->GetCameraCenter();

    const float &fx1 = mpCurrentKeyFrame->fx;
    const float &fy1 = mpCurrentKeyFrame->fy;
    const float &cx1 = mpCurrentKeyFrame->cx;
    const float &cy1 = mpCurrentKeyFrame->cy;
    const float &invfx1 = mpCurrentKeyFrame->invfx;
    const float &invfy1 = mpCurrentKeyFrame->invfy;

    const float ratioFactor = 1.5f * mpCurrentKeyFrame->mfScaleFactor;

    int nnew = 0;

    // Search matches with epipolar restriction and triangulate
    for (size_t i = 0; i < vpNeighKFs.size(); i++)
    {
        if (i > 0 && CheckNewKeyFrames())
            return;

        KeyFrame *pKF2 = vpNeighKFs[i];

        // Check first that baseline is not too short
        cv::Mat Ow2 = pKF2->GetCameraCenter();
        cv::Mat vBaseline = Ow2 - Ow1;
        const float baseline = cv::norm(vBaseline);

        if (!mbMonocular)
        {
            if (baseline < pKF2->mb)
                continue;
        }
        else
        {
            const float medianDepthKF2 = pKF2->ComputeSceneMedianDepth(2);
            const float ratioBaselineDepth = baseline / medianDepthKF2;

            if (ratioBaselineDepth < 0.01)
                continue;
        }

        // Compute Fundamental Matrix
        cv::Mat F12 = ComputeF12(mpCurrentKeyFrame, pKF2);

        // Search matches that fullfil epipolar constraint
        vector<pair<size_t, size_t>> vMatchedIndices;
        matcher.SearchForTriangulation(mpCurrentKeyFrame, pKF2, F12, vMatchedIndices, false);

        cv::Mat Rcw2 = pKF2->GetRotation();
        cv::Mat Rwc2 = Rcw2.t();
        cv::Mat tcw2 = pKF2->GetTranslation();
        cv::Mat Tcw2(3, 4, CV_32F);
        Rcw2.copyTo(Tcw2.colRange(0, 3));
        tcw2.copyTo(Tcw2.col(3));

        const float &fx2 = pKF2->fx;
        const float &fy2 = pKF2->fy;
        const float &cx2 = pKF2->cx;
        const float &cy2 = pKF2->cy;
        const float &invfx2 = pKF2->invfx;
        const float &invfy2 = pKF2->invfy;

        // Triangulate each match
        const int nmatches = vMatchedIndices.size();
        for (int ikp = 0; ikp < nmatches; ikp++)
        {
            const int &idx1 = vMatchedIndices[ikp].first;
            const int &idx2 = vMatchedIndices[ikp].second;

            const cv::KeyPoint &kp1 = mpCurrentKeyFrame->mvKeysUn[idx1];
            const float kp1_ur = mpCurrentKeyFrame->mvuRight[idx1];
            bool bStereo1 = kp1_ur >= 0;

            const cv::KeyPoint &kp2 = pKF2->mvKeysUn[idx2];
            const float kp2_ur = pKF2->mvuRight[idx2];
            bool bStereo2 = kp2_ur >= 0;

            // Check parallax between rays
            cv::Mat xn1 = (cv::Mat_<float>(3, 1) << (kp1.pt.x - cx1) * invfx1, (kp1.pt.y - cy1) * invfy1, 1.0);
            cv::Mat xn2 = (cv::Mat_<float>(3, 1) << (kp2.pt.x - cx2) * invfx2, (kp2.pt.y - cy2) * invfy2, 1.0);

            cv::Mat ray1 = Rwc1 * xn1;
            cv::Mat ray2 = Rwc2 * xn2;
            const float cosParallaxRays = ray1.dot(ray2) / (cv::norm(ray1) * cv::norm(ray2));

            float cosParallaxStereo = cosParallaxRays + 1;
            float cosParallaxStereo1 = cosParallaxStereo;
            float cosParallaxStereo2 = cosParallaxStereo;

            if (bStereo1)
                cosParallaxStereo1 = cos(2 * atan2(mpCurrentKeyFrame->mb / 2, mpCurrentKeyFrame->mvDepth[idx1]));
            else if (bStereo2)
                cosParallaxStereo2 = cos(2 * atan2(pKF2->mb / 2, pKF2->mvDepth[idx2]));

            cosParallaxStereo = min(cosParallaxStereo1, cosParallaxStereo2);

            cv::Mat x3D;
            if (cosParallaxRays < cosParallaxStereo && cosParallaxRays > 0 && (bStereo1 || bStereo2 || cosParallaxRays < 0.9998))
            {
                // Linear Triangulation Method
                cv::Mat A(4, 4, CV_32F);
                A.row(0) = xn1.at<float>(0) * Tcw1.row(2) - Tcw1.row(0);
                A.row(1) = xn1.at<float>(1) * Tcw1.row(2) - Tcw1.row(1);
                A.row(2) = xn2.at<float>(0) * Tcw2.row(2) - Tcw2.row(0);
                A.row(3) = xn2.at<float>(1) * Tcw2.row(2) - Tcw2.row(1);

                cv::Mat w, u, vt;
                cv::SVD::compute(A, w, u, vt, cv::SVD::MODIFY_A | cv::SVD::FULL_UV);

                x3D = vt.row(3).t();

                if (x3D.at<float>(3) == 0)
                    continue;

                // Euclidean coordinates
                x3D = x3D.rowRange(0, 3) / x3D.at<float>(3);
            }
            else if (bStereo1 && cosParallaxStereo1 < cosParallaxStereo2)
            {
                x3D = mpCurrentKeyFrame->UnprojectStereo(idx1);
            }
            else if (bStereo2 && cosParallaxStereo2 < cosParallaxStereo1)
            {
                x3D = pKF2->UnprojectStereo(idx2);
            }
            else
                continue; // No stereo and very low parallax

            cv::Mat x3Dt = x3D.t();

            // Check triangulation in front of cameras
            float z1 = Rcw1.row(2).dot(x3Dt) + tcw1.at<float>(2);
            if (z1 <= 0)
                continue;

            float z2 = Rcw2.row(2).dot(x3Dt) + tcw2.at<float>(2);
            if (z2 <= 0)
                continue;

            // Check reprojection error in first keyframe
            const float &sigmaSquare1 = mpCurrentKeyFrame->mvLevelSigma2[kp1.octave];
            const float x1 = Rcw1.row(0).dot(x3Dt) + tcw1.at<float>(0);
            const float y1 = Rcw1.row(1).dot(x3Dt) + tcw1.at<float>(1);
            const float invz1 = 1.0 / z1;

            if (!bStereo1)
            {
                float u1 = fx1 * x1 * invz1 + cx1;
                float v1 = fy1 * y1 * invz1 + cy1;
                float errX1 = u1 - kp1.pt.x;
                float errY1 = v1 - kp1.pt.y;
                if ((errX1 * errX1 + errY1 * errY1) > 5.991 * sigmaSquare1)
                    continue;
            }
            else
            {
                float u1 = fx1 * x1 * invz1 + cx1;
                float u1_r = u1 - mpCurrentKeyFrame->mbf * invz1;
                float v1 = fy1 * y1 * invz1 + cy1;
                float errX1 = u1 - kp1.pt.x;
                float errY1 = v1 - kp1.pt.y;
                float errX1_r = u1_r - kp1_ur;
                if ((errX1 * errX1 + errY1 * errY1 + errX1_r * errX1_r) > 7.8 * sigmaSquare1)
                    continue;
            }

            // Check reprojection error in second keyframe
            const float sigmaSquare2 = pKF2->mvLevelSigma2[kp2.octave];
            const float x2 = Rcw2.row(0).dot(x3Dt) + tcw2.at<float>(0);
            const float y2 = Rcw2.row(1).dot(x3Dt) + tcw2.at<float>(1);
            const float invz2 = 1.0 / z2;
            if (!bStereo2)
            {
                float u2 = fx2 * x2 * invz2 + cx2;
                float v2 = fy2 * y2 * invz2 + cy2;
                float errX2 = u2 - kp2.pt.x;
                float errY2 = v2 - kp2.pt.y;
                if ((errX2 * errX2 + errY2 * errY2) > 5.991 * sigmaSquare2)
                    continue;
            }
            else
            {
                float u2 = fx2 * x2 * invz2 + cx2;
                float u2_r = u2 - mpCurrentKeyFrame->mbf * invz2;
                float v2 = fy2 * y2 * invz2 + cy2;
                float errX2 = u2 - kp2.pt.x;
                float errY2 = v2 - kp2.pt.y;
                float errX2_r = u2_r - kp2_ur;
                if ((errX2 * errX2 + errY2 * errY2 + errX2_r * errX2_r) > 7.8 * sigmaSquare2)
                    continue;
            }

            // Check scale consistency
            cv::Mat normal1 = x3D - Ow1;
            float dist1 = cv::norm(normal1);

            cv::Mat normal2 = x3D - Ow2;
            float dist2 = cv::norm(normal2);

            if (dist1 == 0 || dist2 == 0)
                continue;

            const float ratioDist = dist2 / dist1;
            const float ratioOctave = mpCurrentKeyFrame->mvScaleFactors[kp1.octave] / pKF2->mvScaleFactors[kp2.octave];

            /*if(fabs(ratioDist-ratioOctave)>ratioFactor)
                continue;*/
            if (ratioDist * ratioFactor < ratioOctave || ratioDist > ratioOctave * ratioFactor)
                continue;

            // Edge-SLAM: added wchThread parameter
            // Triangulation is succesfull
            MapPoint *pMP = new MapPoint(x3D, mpCurrentKeyFrame, mpMap, 2);

            pMP->AddObservation(mpCurrentKeyFrame, idx1);
            pMP->AddObservation(pKF2, idx2);

            mpCurrentKeyFrame->AddMapPoint(pMP, idx1);
            pKF2->AddMapPoint(pMP, idx2);

            pMP->ComputeDistinctiveDescriptors();

            pMP->UpdateNormalAndDepth();

            mpMap->AddMapPoint(pMP);
            mlpRecentAddedMapPoints.push_back(pMP);

            nnew++;
        }
    }
}

void LocalMapping::SearchInNeighbors()
{
    // Retrieve neighbor keyframes
    int nn = 10;
    if (mbMonocular)
        nn = 20;
    const vector<KeyFrame *> vpNeighKFs = mpCurrentKeyFrame->GetBestCovisibilityKeyFrames(nn);
    vector<KeyFrame *> vpTargetKFs;
    if (edgeNumber == 2)
    {
        cout << "here 3.1.1\n";
    }
    for (vector<KeyFrame *>::const_iterator vit = vpNeighKFs.begin(), vend = vpNeighKFs.end(); vit != vend; vit++)
    {
        KeyFrame *pKFi = *vit;
        if (pKFi->isBad() || pKFi->mnFuseTargetForKF == mpCurrentKeyFrame->mnId)
            continue;
        vpTargetKFs.push_back(pKFi);
        pKFi->mnFuseTargetForKF = mpCurrentKeyFrame->mnId;

        // Extend to some second neighbors
        const vector<KeyFrame *> vpSecondNeighKFs = pKFi->GetBestCovisibilityKeyFrames(5);
        for (vector<KeyFrame *>::const_iterator vit2 = vpSecondNeighKFs.begin(), vend2 = vpSecondNeighKFs.end(); vit2 != vend2; vit2++)
        {
            KeyFrame *pKFi2 = *vit2;
            if (pKFi2->isBad() || pKFi2->mnFuseTargetForKF == mpCurrentKeyFrame->mnId || pKFi2->mnId == mpCurrentKeyFrame->mnId)
                continue;
            vpTargetKFs.push_back(pKFi2);
        }
    }
    if (edgeNumber == 2)
    {
        cout << "here 3.1.2\n";
    }

    // Search matches by projection from current KF in target KFs
    ORBmatcher matcher;
    vector<MapPoint *> vpMapPointMatches = mpCurrentKeyFrame->GetMapPointMatches();
    for (vector<KeyFrame *>::iterator vit = vpTargetKFs.begin(), vend = vpTargetKFs.end(); vit != vend; vit++)
    {
        KeyFrame *pKFi = *vit;

        matcher.Fuse(pKFi, vpMapPointMatches);
    }

    if (edgeNumber == 2)
    {
        cout << "here 3.1.3\n";
    }

    // Search matches by projection from target KFs in current KF
    vector<MapPoint *> vpFuseCandidates;
    vpFuseCandidates.reserve(vpTargetKFs.size() * vpMapPointMatches.size());

    for (vector<KeyFrame *>::iterator vitKF = vpTargetKFs.begin(), vendKF = vpTargetKFs.end(); vitKF != vendKF; vitKF++)
    {
        KeyFrame *pKFi = *vitKF;

        vector<MapPoint *> vpMapPointsKFi = pKFi->GetMapPointMatches();

        for (vector<MapPoint *>::iterator vitMP = vpMapPointsKFi.begin(), vendMP = vpMapPointsKFi.end(); vitMP != vendMP; vitMP++)
        {
            MapPoint *pMP = *vitMP;
            if (!pMP)
                continue;
            if (pMP->isBad() || pMP->mnFuseCandidateForKF == mpCurrentKeyFrame->mnId)
                continue;
            pMP->mnFuseCandidateForKF = mpCurrentKeyFrame->mnId;
            vpFuseCandidates.push_back(pMP);
        }
    }

    if (edgeNumber == 2)
    {
        cout << "here 3.1.4\n";
    }

    matcher.Fuse(mpCurrentKeyFrame, vpFuseCandidates);
    if (edgeNumber == 2)
    {
        cout << "here 3.1.4.1\n";
    }
    // Update points
    vpMapPointMatches = mpCurrentKeyFrame->GetMapPointMatches();
    if (edgeNumber == 2)
    {
        cout << "here 3.1.4.2\n";
    }

    // Need to change this
    if (activeEdge)
    {
        for (size_t i = 0, iend = vpMapPointMatches.size(); i < iend; i++)
        {
            MapPoint *pMP = vpMapPointMatches[i];
            if (pMP)
            {
                if (!pMP->isBad())
                {
                    pMP->ComputeDistinctiveDescriptors();
                    pMP->UpdateNormalAndDepth();
                }
            }
        }
    }
    if (edgeNumber == 2)
    {
        cout << "here 3.1.5\n";
    }

    // Update connections in covisibility graph
    mpCurrentKeyFrame->UpdateConnections(false);
    if (edgeNumber == 2)
    {
        cout << "here 3.1.6\n";
    }
}

cv::Mat LocalMapping::ComputeF12(KeyFrame *&pKF1, KeyFrame *&pKF2)
{
    cv::Mat R1w = pKF1->GetRotation();
    cv::Mat t1w = pKF1->GetTranslation();
    cv::Mat R2w = pKF2->GetRotation();
    cv::Mat t2w = pKF2->GetTranslation();

    cv::Mat R12 = R1w * R2w.t();
    cv::Mat t12 = -R1w * R2w.t() * t2w + t1w;

    cv::Mat t12x = SkewSymmetricMatrix(t12);

    const cv::Mat &K1 = pKF1->mK;
    const cv::Mat &K2 = pKF2->mK;

    return K1.t().inv() * t12x * R12 * K2.inv();
}

void LocalMapping::RequestStop()
{
    unique_lock<mutex> lock(mMutexStop);
    mbStopRequested = true;
    unique_lock<mutex> lock2(mMutexNewKFs);
    mbAbortBA = true;
}

bool LocalMapping::Stop()
{
    unique_lock<mutex> lock(mMutexStop);
    if (mbStopRequested && !mbNotStop)
    {
        mbStopped = true;
        cout << "Local Mapping STOP" << endl;
        return true;
    }

    return false;
}

bool LocalMapping::isStopped()
{
    unique_lock<mutex> lock(mMutexStop);
    return mbStopped;
}

bool LocalMapping::stopRequested()
{
    unique_lock<mutex> lock(mMutexStop);
    return mbStopRequested;
}

void LocalMapping::Release()
{
    unique_lock<mutex> lock(mMutexStop);
    unique_lock<mutex> lock2(mMutexFinish);
    if (mbFinished)
        return;
    mbStopped = false;
    mbStopRequested = false;
    for (list<KeyFrame *>::iterator lit = mlNewKeyFrames.begin(), lend = mlNewKeyFrames.end(); lit != lend; lit++)
        delete *lit;
    mlNewKeyFrames.clear();

    cout << "Local Mapping RELEASE" << endl;
}

bool LocalMapping::AcceptKeyFrames()
{
    unique_lock<mutex> lock(mMutexAccept);
    return mbAcceptKeyFrames;
}

void LocalMapping::SetAcceptKeyFrames(bool flag)
{
    unique_lock<mutex> lock(mMutexAccept);
    mbAcceptKeyFrames = flag;
}

bool LocalMapping::SetNotStop(bool flag)
{
    unique_lock<mutex> lock(mMutexStop);

    if (flag && mbStopped)
        return false;

    mbNotStop = flag;

    return true;
}

void LocalMapping::InterruptBA()
{
    mbAbortBA = true;
}

void LocalMapping::KeyFrameCulling()
{
    // Check redundant keyframes (only local keyframes)
    // A keyframe is considered redundant if the 90% of the MapPoints it sees, are seen
    // in at least other 3 keyframes (in the same or finer scale)
    // We only consider close stereo points
    vector<KeyFrame *> vpLocalKeyFrames = mpCurrentKeyFrame->GetVectorCovisibleKeyFrames();

    for (vector<KeyFrame *>::iterator vit = vpLocalKeyFrames.begin(), vend = vpLocalKeyFrames.end(); vit != vend; vit++)
    {
        KeyFrame *pKF = *vit;
        if (pKF->mnId == 0)
            continue;
        const vector<MapPoint *> vpMapPoints = pKF->GetMapPointMatches();

        int nObs = 3;
        const int thObs = nObs;
        int nRedundantObservations = 0;
        int nMPs = 0;
        for (size_t i = 0, iend = vpMapPoints.size(); i < iend; i++)
        {
            MapPoint *pMP = vpMapPoints[i];
            if (pMP)
            {
                if (!pMP->isBad())
                {
                    if (!mbMonocular)
                    {
                        if (pKF->mvDepth[i] > pKF->mThDepth || pKF->mvDepth[i] < 0)
                            continue;
                    }

                    nMPs++;
                    if (pMP->Observations() > thObs)
                    {
                        const int &scaleLevel = pKF->mvKeysUn[i].octave;
                        const map<KeyFrame *, size_t> observations = pMP->GetObservations();
                        int nObs = 0;
                        for (map<KeyFrame *, size_t>::const_iterator mit = observations.begin(), mend = observations.end(); mit != mend; mit++)
                        {
                            KeyFrame *pKFi = mit->first;
                            if (pKFi == pKF)
                                continue;
                            const int &scaleLeveli = pKFi->mvKeysUn[mit->second].octave;

                            if (scaleLeveli <= scaleLevel + 1)
                            {
                                nObs++;
                                if (nObs >= thObs)
                                    break;
                            }
                        }
                        if (nObs >= thObs)
                        {
                            nRedundantObservations++;
                        }
                    }
                }
            }
        }

        if (nRedundantObservations > 0.9 * nMPs)
            pKF->SetBadFlag();
    }
}

cv::Mat LocalMapping::SkewSymmetricMatrix(const cv::Mat &v)
{
    return (cv::Mat_<float>(3, 3) << 0, -v.at<float>(2), v.at<float>(1),
            v.at<float>(2), 0, -v.at<float>(0),
            -v.at<float>(1), v.at<float>(0), 0);
}

void LocalMapping::RequestReset()
{
    {
        unique_lock<mutex> lock(mMutexReset);
        mbResetRequested = true;
    }

    /* Edge-SLAM: disabled, because local mapping thread is handling the system reset
    while(1)
    {
        {
            unique_lock<mutex> lock2(mMutexReset);
            if(!mbResetRequested)
                break;
        }
        usleep(3000);
    }*/
}

void LocalMapping::ResetIfRequested()
{
    unique_lock<mutex> lock(mMutexReset);
    if (mbResetRequested)
    {
        // Edge-SLAM
        cout << "Starting Edge-SLAM server reset..." << endl;
        // Reset Local Mapping
        cout << "Reseting Local Mapper...";

        mlNewKeyFrames.clear();
        mlpRecentAddedMapPoints.clear();

        // Edge-SLAM: reset Edge-SLAM variables
        // Edge-SLAM: measure
        msLastMUStart = std::chrono::high_resolution_clock::now();
        // Edge-SLAM: map update variables
        msNewKFFlag = false;
        while (!msLatestKFsId.empty())
            msLatestKFsId.pop();
        // Edge-SLAM: relocalization
        msRelocStatus = false;
        msRelocNewFFlag = false;
        usCandidateKFsId.clear();
        vpCandidateKFs.clear();
        // Edge-SLAM
        mnLastKeyFrameId = 0;

        // Edge-SLAM: reset frame/keyframe/map queues
        string data;
        // Keyframe
        while (keyframe_queue.try_dequeue(data))
        {
            data.clear();
        }
        // Frame
        while (frame_queue.try_dequeue(data))
        {
            data.clear();
        }
        // Map
        while (map_queue.try_dequeue(data))
        {
            data.clear();
        }

        // Edge-SLAM: continue reset process
        cout << " done" << endl;
        Reset();

        mbResetRequested = false;

        // Edge-SLAM
        cout << "Edge-SLAM server reset is complete" << endl;
    }
}

// Edge-SLAM
bool LocalMapping::CheckReset()
{
    unique_lock<mutex> lock(mMutexReset);
    return mbResetRequested;
}

void LocalMapping::RequestFinish()
{
    unique_lock<mutex> lock(mMutexFinish);
    mbFinishRequested = true;
}

bool LocalMapping::CheckFinish()
{
    unique_lock<mutex> lock(mMutexFinish);
    return mbFinishRequested;
}

void LocalMapping::SetFinish()
{
    // Edge-SLAM: terminate TCP threads
    keyframe_socket->~TcpSocket();
    frame_socket->~TcpSocket();
    map_socket->~TcpSocket();
    // This is just a dummy enqueue to unblock the wait_dequeue function in tcp_send()
    map_queue.enqueue("exit");

    unique_lock<mutex> lock(mMutexFinish);
    mbFinished = true;
    unique_lock<mutex> lock2(mMutexStop);
    mbStopped = true;
}

bool LocalMapping::isFinished()
{
    unique_lock<mutex> lock(mMutexFinish);
    return mbFinished;
}

// Edge-SLAM: similar to Tracking::NeedNewKeyFrame(), but customized for local-mapping
bool LocalMapping::NeedNewKeyFrame(KeyFrame *pKF)
{
    // If Local Mapping is freezed by a Loop Closure do not insert keyframes
    if (isStopped() || stopRequested())
        return false;

    // Edge-SLAM: part one of this condition was checked in CreateNewKeyFrame() in tracking on client
    // Edge-SLAM: now checking part two
    const int nKFs = mpMap->KeyFramesInMap();
    // Do not insert keyframes if not enough frames have passed from last relocalisation
    if (!(pKF->GetPassedF()) && nKFs > mMaxFrames)
        return false;

    // Local Mapping accept keyframes?
    bool bLocalMappingIdle = AcceptKeyFrames();

    // Edge-SLAM: checked in NeedNewKeyFrame() function in tracking thread on client
    // Condition 1c: tracking is weak
    // const bool c1c =  mSensor!=System::MONOCULAR && (mnMatchesInliers<nRefMatches*0.25 || bNeedToInsertClose) ;
    // Condition 2: Few tracked points compared to reference keyframe. Lots of visual odometry compared to map matches.
    // const bool c2 = ((mnMatchesInliers<nRefMatches*thRefRatio|| bNeedToInsertClose) && mnMatchesInliers>15);

    // Edge-SLAM: customized for server side
    bool c1a = false;
    bool c1b = false;
    if (pKF->GetNeedNKF() == 1)
    {
        // Condition 1a: More than "MaxFrames" have passed from last keyframe insertion
        c1a = pKF->mnFrameId >= mnLastKeyFrameId + mMaxFrames;
        // Condition 1b: More than "MinFrames" have passed and Local Mapping is idle
        c1b = (pKF->mnFrameId >= mnLastKeyFrameId + mMinFrames && bLocalMappingIdle);
    }

    if (c1a || c1b || (pKF->GetNeedNKF() == 2))
    {
        // If the mapping accepts keyframes, insert keyframe.
        // Otherwise send a signal to interrupt BA
        if (bLocalMappingIdle)
        {
            return true;
        }
        else
        {
            InterruptBA();
            if (!mbMonocular)
            {
                if (KeyframesInQueue() < 3)
                    return true;
                else
                    return false;
            }
            else
                return false;
        }
    }
    else
        return false;
}

// Edge-SLAM
void LocalMapping::frameCallback(const std::string &msg)
{
    // If Local Mapping is freezed by a Loop Closure do not process frame
    if (isStopped() || stopRequested())
        return;

    // Moved from CreateNewKeyFrame() function in tracking thread on client
    if (!SetNotStop(true))
        return;

    Frame *F = new Frame();
    try
    {
        std::stringstream iis(msg);
        boost::archive::text_iarchive iia(iis);
        iia >> F;
    }
    catch (boost::archive::archive_exception e)
    {
        cout << "log,LocalMapping::frameCallback,error: " << e.what() << endl;
        SetNotStop(false);
        return;
    }

    // Relocalization is performed when tracking is lost
    // Track Lost: Query KeyFrame Database for keyframe candidates for relocalisation
    vector<KeyFrame *> caKFs = mpKeyFrameDB->DetectRelocalizationCandidates(F);

    for (vector<KeyFrame *>::iterator it = caKFs.begin(); it != caKFs.end(); it++)
    {
        KeyFrame *cKF = *it;

        // Using unordered set which only accepts unique elements and has fast
        // retrieval times. If insertion was successful, then add keyframe
        if (usCandidateKFsId.insert(cKF->mnId).second)
            vpCandidateKFs.push_back(cKF);
    }

    msNewKFFlag = false;
    msRelocStatus = true;
    msRelocNewFFlag = true;

    cout << "log,LocalMapping::frameCallback,received relocalization request" << endl;

    delete F;

    SetNotStop(false);
}

// Edge-SLAM
void LocalMapping::sendLocalMapUpdate()
{
    cout << "log,LocalMapping::sendLocalMapUpdate,publish local map update" << endl;

    std::vector<std::string> KFsData;

    // If map size is less than localMapSize, send the whole map, else send the lastest of size localMapSize
    long unsigned localMapSize = KF_NUM;
    if (mpMap->KeyFramesInMap() <= localMapSize)
    {
        // Get all keyframes
        vector<KeyFrame *> current_local_map = mpMap->GetAllKeyFrames();

        cout << "log,LocalMapping::sendLocalMapUpdate,keyframes in local map update: ";

        // Iterate through all keyframes in map
        for (vector<KeyFrame *>::iterator mit = current_local_map.begin(); mit != current_local_map.end(); mit++)
        {
            KeyFrame *tKF = *mit;

            std::ostringstream os;
            boost::archive::text_oarchive oa(os);
            oa << tKF;
            KFsData.push_back(os.str());
            os.clear();

            cout << tKF->mnId << " ";
        }

        cout << endl;
    }
    else
    {
        cout << "log,LocalMapping::sendLocalMapUpdate,keyframes in local map update: ";

        // Temporary stack to hold popped KF ids
        stack<long unsigned int> msLocalKFsId;

        // Pop from stack until localMapSize is reached
        long unsigned count = 0;
        while ((count < localMapSize) && (msLatestKFsId.size() > 0))
        {
            KeyFrame *tKF = mpMap->RetrieveKeyFrame(msLatestKFsId.top());
            msLocalKFsId.push(msLatestKFsId.top());
            msLatestKFsId.pop();

            if (tKF)
            {
                std::ostringstream os;
                boost::archive::text_oarchive oa(os);
                oa << tKF;
                KFsData.push_back(os.str());
                os.clear();

                cout << tKF->mnId << " ";

                count++;
            }
        }

        // Clear Latest KF ids stack
        while (!msLatestKFsId.empty())
            msLatestKFsId.pop();

        // Return last latest KF ids from temporary stack to original stack
        while (!msLocalKFsId.empty())
        {
            msLatestKFsId.push(msLocalKFsId.top());
            msLocalKFsId.pop();
        }

        cout << endl;
    }

    if (KFsData.size() > 0)
    {
        std::ostringstream os;
        boost::archive::text_oarchive oa(os);
        oa << KFsData;
        std::string msg;
        msg = os.str();
        os.clear();

        map_queue.enqueue(msg);

        msLastMUStart = std::chrono::high_resolution_clock::now();
    }

    // Clear
    KFsData.clear();

    // Clear relocalization vectors
    usCandidateKFsId.clear();
    vpCandidateKFs.clear();

    // Set to false so no map update is sent until we receive a new keyframe
    msNewKFFlag = false;
}

// Edge-SLAM: relocalization
void LocalMapping::sendRelocMapUpdate()
{
    if (!vpCandidateKFs.empty())
    {
        // Relocalization map

        // Unordered set of KFs Id for duplicate checking
        std::unordered_set<long unsigned int> KFsId;
        // Vector of serialized KFs
        std::vector<std::string> KFsData;

        cout << "log,LocalMapping::sendRelocMapUpdate,keyframes in relocalization map: ";

        for (vector<KeyFrame *>::iterator vpcit = vpCandidateKFs.begin(); vpcit != vpCandidateKFs.end(); vpcit++)
        {
            KeyFrame *vpcKF = *vpcit;

            // Duplicate check
            if (!KFsId.insert(vpcKF->mnId).second)
                continue;

            // first we should send the current keyframe
            std::ostringstream os_current;
            boost::archive::text_oarchive oa_current(os_current);
            oa_current << vpcKF;
            KFsData.push_back(os_current.str());
            os_current.clear();

            cout << vpcKF->mnId << " ";

            // next we get the connected keyframes
            vector<KeyFrame *> current_local_map = vpcKF->GetVectorCovisibleKeyFrames();

            // Counter to limit the number of connected keyframes in reloc map
            int kfCount = 1;
            for (vector<KeyFrame *>::iterator mit = current_local_map.begin(); mit != current_local_map.end(); mit++)
            {
                KeyFrame *tKF = *mit;

                // Duplicate check
                if (!KFsId.insert(tKF->mnId).second)
                    continue;

                // Check connected kf count
                if (kfCount > CONN_KF)
                    break;
                kfCount++;

                std::ostringstream os;
                boost::archive::text_oarchive oa(os);
                oa << tKF;
                KFsData.push_back(os.str());
                os.clear();

                cout << tKF->mnId << " ";
            }
        }

        cout << endl;

        // Attach latest local-map to relocalization map

        cout << "log,LocalMapping::sendRelocMapUpdate,attach latest local-map to relocalization map" << endl;

        // If map size is less than localMapSize, send the whole map, else send the lastest of size localMapSize
        long unsigned localMapSize = KF_NUM;
        if (mpMap->KeyFramesInMap() <= localMapSize)
        {
            // Get all keyframes
            vector<KeyFrame *> current_local_map = mpMap->GetAllKeyFrames();

            cout << "log,LocalMapping::sendRelocMapUpdate,keyframes in local map update: ";

            // Iterate through all keyframes in map
            for (vector<KeyFrame *>::iterator mit = current_local_map.begin(); mit != current_local_map.end(); mit++)
            {
                KeyFrame *tKF = *mit;

                // Duplicate check
                if (!KFsId.insert(tKF->mnId).second)
                    continue;

                std::ostringstream os;
                boost::archive::text_oarchive oa(os);
                oa << tKF;
                KFsData.push_back(os.str());
                os.clear();

                cout << tKF->mnId << " ";
            }

            cout << endl;
        }
        else
        {
            cout << "log,LocalMapping::sendRelocMapUpdate,keyframes in local map update: ";

            // Temporary stack to hold popped KF ids
            stack<long unsigned int> msLocalKFsId;

            // Pop from stack until localMapSize is reached
            long unsigned count = 0;
            while ((count < localMapSize) && (msLatestKFsId.size() > 0))
            {
                KeyFrame *tKF = mpMap->RetrieveKeyFrame(msLatestKFsId.top());
                msLocalKFsId.push(msLatestKFsId.top());
                msLatestKFsId.pop();

                if (tKF)
                {
                    // Duplicate check
                    if (!KFsId.insert(tKF->mnId).second)
                        continue;

                    std::ostringstream os;
                    boost::archive::text_oarchive oa(os);
                    oa << tKF;
                    KFsData.push_back(os.str());
                    os.clear();

                    cout << tKF->mnId << " ";

                    count++;
                }
            }

            // Clear Latest KF ids stack
            while (!msLatestKFsId.empty())
                msLatestKFsId.pop();

            // Return last latest KF ids from temporary stack to original stack
            while (!msLocalKFsId.empty())
            {
                msLatestKFsId.push(msLocalKFsId.top());
                msLocalKFsId.pop();
            }

            cout << endl;
        }

        std::ostringstream os;
        boost::archive::text_oarchive oa(os);
        oa << KFsData;
        std::string msg;
        msg = os.str();
        os.clear();

        map_queue.enqueue(msg);

        // Clear
        KFsId.clear();
        KFsData.clear();

        msLastMUStart = std::chrono::high_resolution_clock::now();
    }
    else
    {
        cout << "log,LocalMapping::sendRelocMapUpdate,publish relocalization map: candidate KF vector is empty" << endl;
    }

    // Clear relocalization vectors
    usCandidateKFsId.clear();
    vpCandidateKFs.clear();
    msRelocNewFFlag = false;
}

// Edge-SLAM: send function to be called on a separate thread
void LocalMapping::tcp_send(moodycamel::BlockingConcurrentQueue<std::string> *messageQueue, TcpSocket *socketObject, std::string name)
{
    std::string msg;
    bool success = true;

    // This is not a busy wait because wait_dequeue function is blocking
    do
    {
        if (!socketObject->checkAlive())
        {
            cout << "log,LocalMapping::tcp_send,terminating thread" << endl;

            break;
        }

        if (success)
            messageQueue->wait_dequeue(msg);

        if ((!msg.empty()) && (msg.compare("exit") != 0))
        {
            // if(name=="subset map"){

            // }
            if (socketObject->sendMessage(msg) == 1)
            {
                success = true;
                msg.clear();

                cout << "log,LocalMapping::tcp_send,sent " << name << endl;
            }
            else
            {
                success = false;
            }
        }
    } while (1);
}

void LocalMapping::startSync()
{
    cout << "Starting Sync" << endl;

    std::vector<std::string> KFsData;
    map_subset_queue_send.enqueue("Start Sync");

    // If map size is less than localMapSize, send the whole map, else send the lastest of size localMapSize
    long unsigned localMapSize = Subset_Map_Size;
    stack<string> KF_stack;
    if (mpMap->KeyFramesInMap() <= localMapSize)
    {
        // Get all keyframes
        vector<KeyFrame *> current_local_map = mpMap->GetAllKeyFrames();

        cout << "log,LocalMapping::startSync,keyframes Map Subset: ";

        // Iterate through all keyframes in map
        for (vector<KeyFrame *>::iterator mit = current_local_map.begin(); mit != current_local_map.end(); mit++)
        {
            KeyFrame *tKF = *mit;

            std::ostringstream os;
            boost::archive::text_oarchive oa(os);
            oa << tKF;
            // KFsData.push_back(os.str());
            std::string msg;
            msg = os.str();
            // map_subset_queue_send.enqueue(msg);
            KF_stack.push(msg);

            os.clear();

            cout << tKF->mnId << " ";
        }

        cout << endl;
    }
    else
    {
        cout << "log,LocalMapping::startSync,keyframes Map Subset: ";

        // Temporary stack to hold popped KF ids
        stack<long unsigned int> msLocalKFsId;

        // Pop from stack until localMapSize is reached
        long unsigned count = 0;
        stack<long unsigned int> msLatestKFsId_copy = msLatestKFsId;
        while ((count < localMapSize) && (msLatestKFsId_copy.size() > 0))
        {
            KeyFrame *tKF = mpMap->RetrieveKeyFrame(msLatestKFsId_copy.top());
            msLocalKFsId.push(msLatestKFsId_copy.top());
            msLatestKFsId_copy.pop();

            if (tKF)
            {
                std::ostringstream os;
                boost::archive::text_oarchive oa(os);
                oa << tKF;
                std::string msg;
                msg = os.str();
                // map_subset_queue_send.enqueue(msg);
                KF_stack.push(msg);
                // KFsData.push_back(os.str());
                os.clear();

                cout << tKF->mnId << " ";

                count++;
            }
        }

        // Clear Latest KF ids stack
        // while (!msLatestKFsId.empty())
        //     msLatestKFsId.pop();

        // // Return last latest KF ids from temporary stack to original stack
        // while (!msLocalKFsId.empty())
        // {
        //     msLatestKFsId.push(msLocalKFsId.top());
        //     msLocalKFsId.pop();
        // }

        cout << endl;
    }

    std::vector<std::string> subVec;

    while (!KF_stack.empty())
    {
        // map_subset_queue_send.enqueue(KF_stack.top());
        subVec.push_back(KF_stack.top());
        KF_stack.pop();
    }


    std::ostringstream os;
    boost::archive::text_oarchive oa(os);
    oa << subVec;
    std::string msg;
    std::string msg_to_send = os.str();
    os.clear();

    map_subset_queue_send(msg_to_send);
    map_subset_queue_send("End Sync");
    // if (KFsData.size() > 0)
    // {
    //     std::ostringstream os;
    //     boost::archive::text_oarchive oa(os);
    //     oa << KFsData;
    //     std::string msg;
    //     msg = os.str();
    //     os.clear();

    //     // map_subset_queue_send.enqueue(msg);

    //     // msLastMUStart = std::chrono::high_resolution_clock::now();
    // }

    // Clear
    KFsData.clear();
    subVec.clear();

    cout << "log,LocalMapping::startSync,subset Sent\n";

    // Clear relocalization vectors
    // usCandidateKFsId.clear();
    // vpCandidateKFs.clear();

    // Set to false so no map update is sent until we receive a new keyframe
    // msNewKFFlag = false;

    // sync = 0;
}

// Edge-SLAM: receive function to be called on a separate thread
void LocalMapping::tcp_receive(moodycamel::ConcurrentQueue<std::string> *messageQueue, TcpSocket *socketObject, unsigned int maxQueueSize, std::string name)
{
    // Here the while(1) won't cause busy waiting as the implementation of receive function is blocking.
    while (1)
    {
        if (!socketObject->checkAlive())
        {
            cout << "log,LocalMapping::tcp_receive,terminating thread" << endl;

            break;
        }
        std::string msg = socketObject->recieveMessage();

        if (!msg.empty())
        {
            // if (msg.length() < 100)
            // {
            //     cout << "Message Received: " << msg << endl;
            //     if (name == "frame")
            //     {
            //         if (msg == "Start Sync")
            //         {
            //             // thread sync_thread(&ORB_SLAM2::LocalMapping::startSync);
            //             // LocalMapping::startSync();
            //             sync=true;
            //             cout << "START SYNC MESSAGE RECEIVED\n";
            //         }
            //     }
            // }

            // else
            // {
            if (messageQueue->size_approx() >= maxQueueSize)
            {
                string data;
                if (messageQueue->try_dequeue(data))
                {
                    data.clear();
                }
            }
            messageQueue->enqueue(msg);

            cout << "log,LocalMapping::tcp_receive,received " << name << endl;
            // }
        }
    }
}

} // namespace ORB_SLAM
