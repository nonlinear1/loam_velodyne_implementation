#include "./DsvLoading/define.h"
#include "./ScanRegistration/MultiScanRegistration.h"
#include "./LaserMapping/LaserMapping.h"

TRANSINFO	calibInfo;

FILE    *dfp;
FILE    *navFp;
int     navLeft, navRight;
int		dsbytesiz = sizeof (point3d)*2 + sizeof (ONEVDNDATA);
int		dFrmNum=0;
int		dFrmNo=0;
bool    camCalibFlag=true;

RMAP	rm;
DMAP	dm;
DMAP	gm, ggm;

ONEDSVFRAME	*onefrm;
ONEDSVFRAME	*originFrm;
std::vector<NAVDATA> nav;
std::list<point2d> trajList;

/* loam部分变量定义 */
pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudIn(new pcl::PointCloud<pcl::PointXYZI>); /* 单帧原始激光数据 */
long long pointcloudTime; /* 单帧原始激光时间戳 */
pcl::visualization::PCLVisualizer viewer("PointCloud Viewer"); /* 单帧原始激光数据可视化窗口 */
pcl::visualization::PCLVisualizer map_viewer("Map Viewer");
bool is_first_visualization = true;
bool is_first_visualization_map = true;

pcl::PointCloud<pcl::PointXYZI> cornerPointsSharp;      ///< sharp corner points cloud
pcl::PointCloud<pcl::PointXYZI> cornerPointsLessSharp;  ///< less sharp corner points cloud
pcl::PointCloud<pcl::PointXYZI> surfPointsFlat;         ///< flat surface points cloud
pcl::PointCloud<pcl::PointXYZI> surfPointsLessFlat;     ///< less flat surface points cloud

pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudMap(new pcl::PointCloud<pcl::PointXYZI>); /* 激光地图 */


class PointCloudViewer;

bool LoadCalibFile (char *szFile)
{
	char			i_line[200];
    FILE			*fp;
	MATRIX			rt;

	fp = fopen (szFile, "r");
	if (!fp) 
		return false;

	rMatrixInit (calibInfo.rot);

	int	i = 0;
	while (1) {
		if (fgets (i_line, 80, fp) == NULL)
			break;

        if (strncmp(i_line, "rot", 3) == 0) {
			strtok (i_line, " ,\t\n");
			calibInfo.ang.x = atof (strtok (NULL, " ,\t\n"))*topi;
			calibInfo.ang.y = atof (strtok (NULL, " ,\t\n"))*topi;
			calibInfo.ang.z = atof (strtok (NULL, " ,\t\n"))*topi;
			createRotMatrix_ZYX (rt, calibInfo.ang.x, calibInfo.ang.y, calibInfo.ang.z);
			rMatrixmulti (calibInfo.rot, rt);
			continue;
		}

        if (strncmp (i_line, "shv", 3) == 0) {
			strtok (i_line, " ,\t\n");
			calibInfo.shv.x = atof (strtok (NULL, " ,\t\n"));
			calibInfo.shv.y = atof (strtok (NULL, " ,\t\n"));
			calibInfo.shv.z = atof (strtok (NULL, " ,\t\n"));
		}
	}
	fclose (fp);

	return true;
}

void SmoothingData ()
{
	int maxcnt = 3;

	for (int y=0; y<rm.len; y++) {
		for (int x=1; x<(rm.wid-1); x++) {
			if (rm.pts[y*rm.wid+(x-1)].i && !rm.pts[y*rm.wid+x].i) {

				int xx;
				for (xx=x+1; xx<rm.wid; xx++) {
					if (rm.pts[y*rm.wid+xx].i)
						break;
				}
				if (xx>=rm.wid)
					continue;
				int cnt = xx-x+1;
				if (cnt>maxcnt) {
					x = xx;
					continue;
				}
				point3fi *p1 = &rm.pts[y*rm.wid+(x-1)];
				point3fi *p2 = &rm.pts[y*rm.wid+xx];
				double dis = ppDistance3fi (p1, p2);
				double rng = max(p2r(p1),p2r(p2));
				double maxdis = min(MAXSMOOTHERR, max (BASEERROR, HORIERRFACTOR*cnt*rng));
				if (dis<maxdis) {
					for (int xxx=x; xxx<xx; xxx++) {
						point3fi *p = &rm.pts[y*rm.wid+xxx];
						p->x = (p2->x-p1->x)/cnt*(xxx-x+1)+p1->x;
						p->y = (p2->y-p1->y)/cnt*(xxx-x+1)+p1->y;
						p->z = (p2->z-p1->z)/cnt*(xxx-x+1)+p1->z;
						p->i = 1;
					}
				}
				x = xx;
			}
		}
	}
}

void CorrectPoints ()
{
	MAT2D	rot1, rot2;

	//transform points to the vehicle frame of onefrm->dsv[0]
	//src: block i; tar: block 0

    //rot2: R_tar^{-1}
	rot2[0][0] = cos (-onefrm->dsv[0].ang.z);
	rot2[0][1] = -sin (-onefrm->dsv[0].ang.z);
	rot2[1][0] = sin (-onefrm->dsv[0].ang.z);
    rot2[1][1] = cos (-onefrm->dsv[0].ang.z);

	for (int i=1; i<BKNUM_PER_FRM; i++) {
		for (int j=0; j<PTNUM_PER_BLK; j++) {
			if (!onefrm->dsv[i].points[j].i)
				continue;

			rotatePoint3fi(onefrm->dsv[i].points[j], calibInfo.rot);
			shiftPoint3fi(onefrm->dsv[i].points[j], calibInfo.shv); 
			rotatePoint3fi(onefrm->dsv[i].points[j], onefrm->dsv[i].rot);

			//rot1: R_tar^{-1}*R_src
			rot1[0][0] = cos (onefrm->dsv[i].ang.z-onefrm->dsv[0].ang.z);
			rot1[0][1] = -sin (onefrm->dsv[i].ang.z-onefrm->dsv[0].ang.z);
			rot1[1][0] = sin (onefrm->dsv[i].ang.z-onefrm->dsv[0].ang.z);
			rot1[1][1] = cos (onefrm->dsv[i].ang.z-onefrm->dsv[0].ang.z);

			//shv: SHV_src-SHV_tar
			point2d shv;
            shv.x = onefrm->dsv[i].shv.x-onefrm->dsv[0].shv.x;
            shv.y = onefrm->dsv[i].shv.y-onefrm->dsv[0].shv.y;

			point2d pp;
			pp.x = onefrm->dsv[i].points[j].x; pp.y = onefrm->dsv[i].points[j].y;
			rotatePoint2d (pp, rot1);	//R_tar^{-1}*R_src*p
			rotatePoint2d (shv, rot2);	//R_tar^{-1}*(SHV_src-SHV_tar)
			shiftPoint2d (pp, shv);		//p'=R_tar^{-1}*R_src*p+R_tar^{-1}*(SHV_src-SHV_tar)
			onefrm->dsv[i].points[j].x = pp.x;
			onefrm->dsv[i].points[j].y = pp.y;
		}
	}

	for (int ry=0; ry<rm.len; ry++) {
		for (int rx=0; rx<rm.wid; rx++) {
			int i=rm.idx[ry*rm.wid+rx].x;
			int j=rm.idx[ry*rm.wid+rx].y;
			if (!i&&!j)
				continue;
			rm.pts[ry*rm.wid+rx] = onefrm->dsv[i].points[j];
		}
	}

    trajList.push_front(point2d{onefrm->dsv[0].shv.x, onefrm->dsv[0].shv.y});
    if (trajList.size() > 2000)
        trajList.pop_back();
}

void ProcessOneFrame ()
{
	GenerateRangeView ();

	CorrectPoints ();

	SmoothingData ();

	memset (rm.regionID, 0, sizeof(int)*rm.wid*rm.len);
	rm.regnum = 0;
	ContourSegger ();
	
    if (rm.regnum) {
		rm.segbuf = new SEGBUF[rm.regnum];
		memset (rm.segbuf, 0, sizeof (SEGBUF)*rm.regnum);
        Region2Seg ();
	}	
    DrawRangeView ();
	
    PredictGloDem (gm,ggm);

    GenerateLocDem (dm, gm);

    UpdateGloDem (gm,dm);

    ExtractRoadCenterline (gm);

    LabelRoadSurface (gm);

    LabelObstacle (gm);

	DrawDem (dm);

	//	DrawDem (gm);

	if (rm.segbuf)
		delete []rm.segbuf;

}

void ConvertPointCloudType ()
{
    laserCloudIn->clear();
    laserCloudIn->width = BKNUM_PER_FRM * LINES_PER_BLK * PNTS_PER_LINE;
    laserCloudIn->height = 1;
    laserCloudIn->is_dense = false;
//    laserCloudIn->resize(laserCloudIn->width * laserCloudIn->height); /* 加入resize后读取数据会出错 */
    for (int i=0; i<BKNUM_PER_FRM; i++) {
        for (int j = 0; j < LINES_PER_BLK; j++) {
            for (int k = 0; k < PNTS_PER_LINE; k++) {
                point3fi *p = &onefrm->dsv[i].points[j * PNTS_PER_LINE + k];
                if (!p->i)
                    continue;
                pcl::PointXYZI single_laserCloudIn;
                single_laserCloudIn.x = p->y; single_laserCloudIn.y = p->z; single_laserCloudIn.z = p->x; single_laserCloudIn.intensity = 1.;
                laserCloudIn->push_back(single_laserCloudIn);
//                std::cout << "p " << p->x << std::endl;
//
//                std::cout << "single_laserCloudIn " << single_laserCloudIn.x << std::endl;
//                std::cout << "laserCloudIn->points " << laserCloudIn->points[k].x << std::endl;
//                if (k==2)
//                    return;
//                float rng = sqrt(sqr(p->x) + sqr(p->y) + sqr(p->z));
//                float angv = asin(p->z / rng);
//                float angh = atan2(p->y, p->x);
            }
        }
    }
    pointcloudTime = onefrm->dsv[0].millisec;
}

void visualizePointCloud ()
{
    viewer.setBackgroundColor(0, 0, 0);
    pcl::visualization::PointCloudColorHandlerCustom<pcl::PointXYZI> red(cornerPointsSharp.makeShared(), 255, 9, 0);
    pcl::visualization::PointCloudColorHandlerCustom<pcl::PointXYZI> green(surfPointsFlat.makeShared(), 0, 255, 0);
    if(is_first_visualization)
    {
        viewer.addPointCloud<pcl::PointXYZI>(laserCloudIn, "Point Cloud");
        viewer.addPointCloud<pcl::PointXYZI>(cornerPointsSharp.makeShared(), red, "CornerPointSharp");
        viewer.addPointCloud<pcl::PointXYZI>(surfPointsFlat.makeShared(), green, "surfPointsFlat");
    }
    else
    {
        viewer.updatePointCloud<pcl::PointXYZI>(laserCloudIn, "Point Cloud");
        viewer.updatePointCloud<pcl::PointXYZI>(cornerPointsSharp.makeShared(), red, "CornerPointSharp");
        viewer.updatePointCloud<pcl::PointXYZI>(surfPointsFlat.makeShared(), green, "surfPointsFlat");
    }
    is_first_visualization = false;
    viewer.setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 1, "Point Cloud");
    viewer.setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 4, "CornerPointSharp");
    viewer.setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 4, "surfPointsFlat");

    viewer.addCoordinateSystem(1.0);
    viewer.spinOnce(100);
}

void visualizeMap ()
{
    map_viewer.setBackgroundColor(0, 0, 0);
    if(is_first_visualization_map)
    {
        map_viewer.addPointCloud<pcl::PointXYZI>(laserCloudMap, "Map");
    }
    else
    {
        map_viewer.updatePointCloud<pcl::PointXYZI>(laserCloudMap, "Map");
    }
    is_first_visualization_map = false;
    map_viewer.setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 1, "Map");

    map_viewer.addCoordinateSystem(1.0);
    map_viewer.spinOnce(100);
}

void ExtractFeatures ()
{
    ConvertPointCloudType();
    loam::MultiScanRegistration multiScan;
    multiScan.process(laserCloudIn, pointcloudTime, cornerPointsSharp, cornerPointsLessSharp, surfPointsLessFlat, surfPointsFlat);

//    std::cout << "cornerPointsSharp.size = " << cornerPointsSharp.points.size() << std::endl;
//    std::cout << "cornerPointsLessSharp.size = " << cornerPointsLessSharp.points.size() << std::endl;
//    std::cout << "surfPointsLessFlat.size = " << surfPointsLessFlat.points.size() << std::endl;
//    std::cout << "surfPointsFlat.size = " << surfPointsFlat.points.size() << std::endl;

//    visualizePointCloud();
}

void LaserOdometry ()
{

}

void LaserMapping ()
{
    loam::LaserMapping laserMapping(0.1);
    laserMapping.process(laserCloudIn, pointcloudTime, nav, laserCloudMap);
    visualizeMap();
}

BOOL ReadOneDsvFrame ()
{
	DWORD	dwReadBytes;
	int		i;
    for (i=0; i<BKNUM_PER_FRM; i++) {
        dwReadBytes = fread((ONEDSVDATA *)&onefrm->dsv[i], 1, dsbytesiz, dfp);
        if ((dsbytesiz != dwReadBytes) || (ferror(dfp))) {
            printf("Error from reading file.\n");
			break;
        }
        createRotMatrix_ZYX(onefrm->dsv[i].rot, onefrm->dsv[i].ang.x, onefrm->dsv[i].ang.y , 0 ) ;

        for (int j=0; j<LINES_PER_BLK; j++) {
            for (int k=0; k<PNTS_PER_LINE; k++) {
                point3fi *p = &onefrm->dsv[i].points[j*PNTS_PER_LINE+k];
                double dis2Vehicle = sqrt(sqr(p->x)+sqr(p->y));
                if (dis2Vehicle < 4.0) {    // m
                    p->i = 0;
                }
            }
        }
    }
    if (camCalibFlag) {
        memcpy(originFrm, onefrm, sizeof(*onefrm));
    }
	if (i<BKNUM_PER_FRM)
        return false;
	else
        return true;
}

LONGLONG myGetFileSize(FILE *f)
{
    // set the file pointer to end of file
    fseeko(f, 0, SEEK_END);
    // get the file size
    LONGLONG retSize = ftello(f);
    // return the file pointer to the begin of file
    rewind(f);
    return retSize;
}

void DrawTraj(IplImage *img)
{
    MAT2D	rot2;
    list<point2d>::iterator iter;
    iter = trajList.begin();
    point2d centerPoint = point2d{iter->x, iter->y};
    point2i centerPixel = point2i{img->height/2, img->width/2};

    for (iter = trajList.begin(); iter != trajList.end(); iter ++) {
        point2d tmpPoint;
        tmpPoint.x = centerPixel.y;
        tmpPoint.y = centerPixel.x;

        //rot2: R_tar^{-1}
        rot2[0][0] = cos (-onefrm->dsv[0].ang.z);
        rot2[0][1] = -sin (-onefrm->dsv[0].ang.z);
        rot2[1][0] = sin (-onefrm->dsv[0].ang.z);
        rot2[1][1] = cos (-onefrm->dsv[0].ang.z);

        //shv: SHV_src-SHV_tar
        point2d shv;
        shv.x = (iter->x - centerPoint.x) / PIXSIZ;
        shv.y = (iter->y - centerPoint.y) / PIXSIZ;

        rotatePoint2d (shv, rot2);          //R_tar^{-1}*(SHV_src-SHV_tar)
        shiftPoint2d (tmpPoint, shv);		//p'=R_tar^{-1}*R_src*p+R_tar^{-1}*(SHV_src-SHV_tar)
        cvCircle(img, cvPoint((int)tmpPoint.x, (int)tmpPoint.y), 3, cv::Scalar(255,255,255), -1, 8);
    }
}


void LoadNav()
{
    int millisec, stat;
    double gx, gy, gz, roll, pitch, yaw;
    while (fscanf(navFp, "%d %lf %lf %lf %lf %lf %lf %d\n", &millisec, &roll, &pitch, &yaw, &gx, &gy, &gz, &stat) != EOF) {
        nav.push_back((NAVDATA){millisec, gx, gy, gz, roll, pitch, yaw, stat});
    }
    navLeft = 0;
    navRight = 0;
    printf("size of NAV: %d\n", int(nav.size()));
}

void DoProcessingOffline(/*P_CGQHDL64E_INFO_MSG *veloData, P_DWDX_INFO_MSG *dwdxData, P_CJDEMMAP_MSG &demMap, P_CJATTRIBUTEMAP_MSG &attributeMap*/)
{
    if (!LoadCalibFile ("/media/sukie/Treasure/Lab/Project/gaobiao/data/vel_hongling.calib")) {
        std::cout << "Invalid calibration file" << std::endl;
        getchar ();
        exit (1);
    }
    // dsv
    if ((dfp = fopen("/media/sukie/Treasure/Lab/Project/gaobiao/data/hongling_round1_2.dsv", "r")) == NULL) {
        printf("File open failure\n");
        getchar ();
        exit (1);
    }
    // video
    cv::VideoCapture cap("/media/sukie/Treasure/Lab/Project/gaobiao/data/2.avi");
    FILE* tsFp = fopen("/media/sukie/Treasure/Lab/Project/gaobiao/data/2.avi.ts", "r");
    if (!cap.isOpened()) {
        printf("Error opening video stream or file.\n");
        getchar();
        exit(1);
    }
    // nav
    if ((navFp = fopen("/media/sukie/Treasure/Lab/Project/gaobiao/data/all.nav", "r")) == NULL) {
        printf("Nav open failure\n");
        getchar ();
        exit (1);
    }
    LoadNav();
    // Camera/Velodyne calib file
    if(!LoadCameraCalib("/media/sukie/Treasure/Lab/Project/gaobiao/data/Sampledata-001-Camera.camera")){
        printf("Open Camera Calibration files fails.\n");
        camCalibFlag = false;
    }

    LONGLONG fileSize = myGetFileSize(dfp);
    dFrmNum = fileSize / (BKNUM_PER_FRM) / dsbytesiz;
	InitRmap (&rm);
	InitDmap (&dm);
	InitDmap (&gm);
	InitDmap (&ggm);
	onefrm= new ONEDSVFRAME[1];
    if (camCalibFlag) {
        originFrm = new ONEDSVFRAME[1];
    }
	IplImage * col = cvCreateImage (cvSize (1024, rm.len*3),IPL_DEPTH_8U,3); 
	CvFont font;
	cvInitFont(&font,CV_FONT_HERSHEY_DUPLEX, 1,1, 0, 2);
    int waitkeydelay=10;
	dFrmNo = 0;

    cv::namedWindow("l_dem");
    cv::moveWindow("l_dem", WIDSIZ*2.3/PIXSIZ, 0);

    std::cout << "size of ONEDSVDATA: " << sizeof(ONEDSVDATA) << std::endl;
    std::cout << "size of MATRIX: " << sizeof(MATRIX) << std::endl;
    while (ReadOneDsvFrame ())
	{

		printf("%d (%d)\n",dFrmNo,dFrmNum);

        ProcessOneFrame ();

        DrawTraj(dm.lmap);

//        cv::Mat vFrame;
//        int vTs;
//        cap >> vFrame;
//        fscanf(tsFp, "%d\n", &vTs);
//        while (vTs < onefrm->dsv[0].millisec) {
//            cap >> vFrame;
//            fscanf(tsFp, "%d\n", &vTs);
//        }

        cv::Mat visImg;
        if (dm.lmap) {
            cv::flip(cv::cvarrToMat(dm.lmap),visImg,0);
            char str[10];
            sprintf (str, "%d", int(onefrm->dsv[0].millisec));
            cv:putText(visImg, str, cvPoint(30,30), cv::FONT_HERSHEY_DUPLEX, 1, cv::Scalar(255,255,255));
            cv::imshow("l_dem",visImg);
        }

        /* ScanRegistration */
        ExtractFeatures();

//        LaserOdometry();

        LaserMapping();

		char WaitKey;
		WaitKey = cvWaitKey(waitkeydelay);
		if (WaitKey==27)
			break;
        dFrmNo++;
    }

    cap.release();
	ReleaseRmap (&rm);
	ReleaseDmap (&dm);
	ReleaseDmap (&gm);
	ReleaseDmap (&ggm);
	cvReleaseImage(&col);
    delete []onefrm;
}

int main (int argc, char *argv[])
{

//    if (argc<3) {
//        printf ("Usage : %s [infile] [calibfile]\n", argv[0]);
//        printf ("[infile] DSV file.\n");
//        printf ("[calibfile] define the calibration parameters of the DSV file.\n");
//        printf ("[outfile] segmentation results to DSVL file.\n");
//        printf ("[seglog] data association results to LOG file.\n");
//        printf ("[videooutflg] 1: output video to default files, 0: no output.\n");
//        exit(1);
//    }

    DoProcessingOffline ();

    printf ("Done.\n");

    fclose(dfp);

    return 0;
}
