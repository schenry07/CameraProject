
#include <iostream>
#include <algorithm>
#include <numeric>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "camFusion.hpp"
#include "dataStructures.h"

using namespace std;


// Create groups of Lidar points whose projection into the camera falls into the same bounding box
void clusterLidarWithROI(std::vector<BoundingBox> &boundingBoxes, std::vector<LidarPoint> &lidarPoints, float shrinkFactor, cv::Mat &P_rect_xx, cv::Mat &R_rect_xx, cv::Mat &RT)
{
    // loop over all Lidar points and associate them to a 2D bounding box
    cv::Mat X(4, 1, cv::DataType<double>::type);
    cv::Mat Y(3, 1, cv::DataType<double>::type);

    for (auto it1 = lidarPoints.begin(); it1 != lidarPoints.end(); ++it1)
    {
        // assemble vector for matrix-vector-multiplication
        X.at<double>(0, 0) = it1->x;
        X.at<double>(1, 0) = it1->y;
        X.at<double>(2, 0) = it1->z;
        X.at<double>(3, 0) = 1;

        // project Lidar point into camera
        Y = P_rect_xx * R_rect_xx * RT * X;
        cv::Point pt;
        pt.x = Y.at<double>(0, 0) / Y.at<double>(0, 2); // pixel coordinates
        pt.y = Y.at<double>(1, 0) / Y.at<double>(0, 2);

        vector<vector<BoundingBox>::iterator> enclosingBoxes; // pointers to all bounding boxes which enclose the current Lidar point
        for (vector<BoundingBox>::iterator it2 = boundingBoxes.begin(); it2 != boundingBoxes.end(); ++it2)
        {
            // shrink current bounding box slightly to avoid having too many outlier points around the edges
            cv::Rect smallerBox;
            smallerBox.x = (*it2).roi.x + shrinkFactor * (*it2).roi.width / 2.0;
            smallerBox.y = (*it2).roi.y + shrinkFactor * (*it2).roi.height / 2.0;
            smallerBox.width = (*it2).roi.width * (1 - shrinkFactor);
            smallerBox.height = (*it2).roi.height * (1 - shrinkFactor);

            // check wether point is within current bounding box
            if (smallerBox.contains(pt))
            {
                enclosingBoxes.push_back(it2);
            }

        } // eof loop over all bounding boxes

        // check wether point has been enclosed by one or by multiple boxes
        if (enclosingBoxes.size() == 1)
        { 
            // add Lidar point to bounding box
            enclosingBoxes[0]->lidarPoints.push_back(*it1);
        }

    } // eof loop over all Lidar points
}


void show3DObjects(std::vector<BoundingBox> &boundingBoxes, cv::Size worldSize, cv::Size imageSize, bool bWait)
{
    // create topview image
    cv::Mat topviewImg(imageSize, CV_8UC3, cv::Scalar(255, 255, 255));

    for(auto it1=boundingBoxes.begin(); it1!=boundingBoxes.end(); ++it1)
    {
        // create randomized color for current 3D object
        cv::RNG rng(it1->boxID);
        cv::Scalar currColor = cv::Scalar(rng.uniform(0,150), rng.uniform(0, 150), rng.uniform(0, 150));

        // plot Lidar points into top view image
        int top=1e8, left=1e8, bottom=0.0, right=0.0; 
        float xwmin=1e8, ywmin=1e8, ywmax=-1e8;
        for (auto it2 = it1->lidarPoints.begin(); it2 != it1->lidarPoints.end(); ++it2)
        {
            // world coordinates
            float xw = (*it2).x; // world position in m with x facing forward from sensor
            float yw = (*it2).y; // world position in m with y facing left from sensor
            xwmin = xwmin<xw ? xwmin : xw;
            ywmin = ywmin<yw ? ywmin : yw;
            ywmax = ywmax>yw ? ywmax : yw;

            // top-view coordinates
            int y = (-xw * imageSize.height / worldSize.height) + imageSize.height;
            int x = (-yw * imageSize.width / worldSize.width) + imageSize.width / 2;

            // find enclosing rectangle
            top = top<y ? top : y;
            left = left<x ? left : x;
            bottom = bottom>y ? bottom : y;
            right = right>x ? right : x;

            // draw individual point
            cv::circle(topviewImg, cv::Point(x, y), 4, currColor, -1);
        }

        // draw enclosing rectangle
        cv::rectangle(topviewImg, cv::Point(left, top), cv::Point(right, bottom),cv::Scalar(0,0,0), 2);

        // augment object with some key data
        char str1[200], str2[200];
        sprintf(str1, "id=%d, #pts=%d", it1->boxID, (int)it1->lidarPoints.size());
        putText(topviewImg, str1, cv::Point2f(left-250, bottom+50), cv::FONT_ITALIC, 2, currColor);
        sprintf(str2, "xmin=%2.2f m, yw=%2.2f m", xwmin, ywmax-ywmin);
        putText(topviewImg, str2, cv::Point2f(left-250, bottom+125), cv::FONT_ITALIC, 2, currColor);  
    }

    // plot distance markers
    float lineSpacing = 2.0; // gap between distance markers
    int nMarkers = floor(worldSize.height / lineSpacing);
    for (size_t i = 0; i < nMarkers; ++i)
    {
        int y = (-(i * lineSpacing) * imageSize.height / worldSize.height) + imageSize.height;
        cv::line(topviewImg, cv::Point(0, y), cv::Point(imageSize.width, y), cv::Scalar(255, 0, 0));
    }

    // display image
    string windowName = "3D Objects";
    cv::namedWindow(windowName, 1);
    cv::imshow(windowName, topviewImg);

    if(bWait)
    {
        cv::waitKey(0); // wait for key to be pressed
    }
}


// associate a given bounding box with the keypoints it contains
void clusterKptMatchesWithROI(BoundingBox &boundingBox, std::vector<cv::KeyPoint> &kptsPrev, std::vector<cv::KeyPoint> &kptsCurr, std::vector<cv::DMatch> &kptMatches)
{
    for (auto it1 = kptMatches.begin() + 1; it1 != kptMatches.end(); ++it1)
    { // inner kpt.-loop
        cv::KeyPoint kpOuterCurr = kptsCurr.at(it1->trainIdx);
        cv::KeyPoint kpOuterPrev = kptsPrev.at(it1->queryIdx);

        cv::Rect roi = boundingBox.roi;
        if (roi.contains(kpOuterCurr.pt)){
            //cout << " roi contins matches: x = " << kpOuterCurr.pt.x << "y = " << kpOuterCurr.pt.y << endl; 
            boundingBox.kptMatches.push_back(*it1);
            boundingBox.keypoints.push_back(kpOuterCurr);
        }


    }
}


// Compute time-to-collision (TTC) based on keypoint correspondences in successive images
void computeTTCCamera(std::vector<cv::KeyPoint> &kptsPrev, std::vector<cv::KeyPoint> &kptsCurr, 
                      std::vector<cv::DMatch> kptMatches, double frameRate, double &TTC, cv::Mat *visImg)
{

    // compute distance ratios between all matched keypoints
    vector<double> distRatios; // stores the distance ratios for all keypoints between curr. and prev. frame
    for (auto it1 = kptMatches.begin(); it1 != kptMatches.end() - 1; ++it1)
    { // outer kpt. loop

        // get current keypoint and its matched partner in the prev. frame
        cv::KeyPoint kpOuterCurr = kptsCurr.at(it1->trainIdx);
        cv::KeyPoint kpOuterPrev = kptsPrev.at(it1->queryIdx);

        for (auto it2 = kptMatches.begin() + 1; it2 != kptMatches.end(); ++it2)
        { // inner kpt.-loop

            double minDist = 100.0; // min. required distance

            // get next keypoint and its matched partner in the prev. frame
            cv::KeyPoint kpInnerCurr = kptsCurr.at(it2->trainIdx);
            cv::KeyPoint kpInnerPrev = kptsPrev.at(it2->queryIdx);

            // compute distances and distance ratios
            double distCurr = cv::norm(kpOuterCurr.pt - kpInnerCurr.pt);
            double distPrev = cv::norm(kpOuterPrev.pt - kpInnerPrev.pt);

            if (distPrev > std::numeric_limits<double>::epsilon() && distCurr >= minDist)
            { // avoid division by zero

                double distRatio = distCurr / distPrev;
                distRatios.push_back(distRatio);
            }
        } // eof inner loop over all matched kpts
    }     // eof outer loop over all matched kpts

    // only continue if list of distance ratios is not empty
    if (distRatios.size() == 0)
    {
        TTC = NAN;
        return;
    }

    // compute camera-based TTC from distance ratios
        // STUDENT TASK (replacement for meanDistRatio)

	int int_median_index;
	sort(distRatios.begin(), distRatios.end());

	double medianDistRatio;
	if (distRatios.size() % 2 == 1) {
		int_median_index = (distRatios.size() - 1) / 2;
		medianDistRatio = distRatios[int_median_index];
	}
	else {
		int_median_index = distRatios.size() / 2;
		medianDistRatio = (distRatios[int_median_index-1] + distRatios[int_median_index])/2.0;
	}
  
    double dT = 1 / frameRate;
    TTC = -dT / (1 - medianDistRatio);
}


void computeTTCLidar(std::vector<LidarPoint> &lidarPointsPrev,
                     std::vector<LidarPoint> &lidarPointsCurr, double frameRate, double &TTC)
{
    // ...
    double Xcurrmin = 1e9;
    std::vector<double> Distance_array;
    std::vector<double> X_val_curr;
    std::vector<double> X_val_prev;
    for (auto it1 = lidarPointsCurr.begin();it1 != lidarPointsCurr.end();++it1)
    {
        X_val_curr.push_back(it1->x);
        //cout << "X_val_curr"<< endl;
        //cout << it1->x << endl;

        for (auto it2 = lidarPointsPrev.begin();it2 != lidarPointsPrev.end();++it2)
        {                //cout << "X_val_prev"<< endl;
            //cout << it2->x << endl;
        X_val_prev.push_back(it2->x);
            Distance_array.push_back(it2->x-it1->x);
        }          
    }

    sort(Distance_array.begin(),Distance_array.end());
    sort(X_val_curr.begin(),X_val_curr.end());
    sort(X_val_prev.begin(),X_val_prev.end());

    //cout << X_val_curr[0] << X_val_curr[5] << X_val_curr[10] << X_val_curr[15] << endl;
    //cout << X_val_prev[0] << X_val_prev[5] << X_val_prev[10] << X_val_prev[15] << endl;
    //cout << Distance_array[0] << Distance_array[5] << Distance_array[10] << Distance_array[15] << endl;
    //cout << Distance_array[Distance_array.size()/2-1]<< Distance_array[Distance_array.size()/2]<< endl;
    


    double median_velocity = (X_val_prev[5] - X_val_curr[5]);
    TTC = X_val_curr[5] /  (median_velocity*frameRate);
    //cout << TTC << endl;
    //cout << median_velocity;
    
}


void matchBoundingBoxes(std::vector<cv::DMatch> &matches, map<int, int> &bbBestMatches, DataFrame &prevFrame, DataFrame &currFrame)
{
	int count = 1;
	bbBestMatches.insert({ 1, count });
	bbBestMatches.insert({ 3, 3 });
	bbBestMatches.insert({ 5, 3 });

	//first iterate through matches
	for (int i = 0; i < matches.size(); ++i) {

		//then iterate through previous frame bounding boxes
		for (int j = 0; j < prevFrame.boundingBoxes.size(); ++j) {

			//then iterate through previous frame bounding boxes
			for (int k = 0; k < currFrame.boundingBoxes.size(); ++k) {

				cv::Rect roi_prev = prevFrame.boundingBoxes[j].roi;
				cv::Rect roi_curr = currFrame.boundingBoxes[i].roi;

				int query_ind = matches[i].queryIdx;
				int train_ind = matches[i].trainIdx;

				cv::KeyPoint pt_previous_frame;
				pt_previous_frame = prevFrame.keypoints[query_ind];

				cv::KeyPoint pt_current_frame;
				pt_current_frame = prevFrame.keypoints[train_ind];

				if (roi_prev.contains(pt_previous_frame.pt) && roi_curr.contains(pt_current_frame.pt)) {
					int prev_box_id = prevFrame.boundingBoxes[j].boxID;
					int curr_box_id = currFrame.boundingBoxes[k].boxID;
					bbBestMatches.insert({ prev_box_id, curr_box_id });
				}
			}

		}
	}
}
