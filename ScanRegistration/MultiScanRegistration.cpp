#include "MultiScanRegistration.h"
#include "math_utils.h"

//#include <pcl_conversions/pcl_conversions.h>


namespace loam {

MultiScanMapper::MultiScanMapper(const float& lowerBound,
                                 const float& upperBound,
                                 const uint16_t& nScanRings)
    : _lowerBound(lowerBound),
      _upperBound(upperBound),
      _nScanRings(nScanRings),
      _factor((nScanRings - 1) / (upperBound - lowerBound))
{

}

void MultiScanMapper::set(const float &lowerBound,
                          const float &upperBound,
                          const uint16_t &nScanRings)
{
  _lowerBound = lowerBound;
  _upperBound = upperBound;
  _nScanRings = nScanRings;
  _factor = (nScanRings - 1) / (upperBound - lowerBound);
}



int MultiScanMapper::getRingForAngle(const float& angle) {
//  return int(((angle * 180 / M_PI) - _lowerBound) * (63. / 26.8));
  return int(((angle * 180 / M_PI) - _lowerBound) * _factor + 0.5);
}

MultiScanRegistration::MultiScanRegistration(const MultiScanMapper& scanMapper)
    : _scanMapper(scanMapper)
{};


void MultiScanRegistration::process(const pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudIn,
        const long long& scanTime,
        pcl::PointCloud<pcl::PointXYZI>& _cornerPointsSharp,
        pcl::PointCloud<pcl::PointXYZI>& _cornerPointsLessSharp,
        pcl::PointCloud<pcl::PointXYZI>& _surfPointsLessFlat,
        pcl::PointCloud<pcl::PointXYZI>& _surfPointsFlat)
{
  // determine size of current pointcloud frame
  size_t cloudSize = laserCloudIn->width * laserCloudIn->height;

  // determine scan start and end orientations
  float startOri = -std::atan2(laserCloudIn->points[0].x, laserCloudIn->points[0].z);
  float endOri = -std::atan2(laserCloudIn->points[cloudSize - 1].x, laserCloudIn->points[cloudSize - 1].z) + 2 * float(M_PI);
  if (endOri - startOri > 3 * M_PI) endOri -= 2 * M_PI;
  else if (endOri - startOri < M_PI) endOri += 2 * M_PI;

  std::cout << "startOri -> " << startOri << ", endOri" << endOri << std::endl;

  // clear all scanline points
  std::for_each(_laserCloudScans.begin(), _laserCloudScans.end(), [](auto&&v) {v.clear(); }); 

 
  // extract valid points from input cloud
  bool halfPassed = false;
  pcl::PointXYZI point;
  _laserCloudScans.resize(_scanMapper.getNumberOfScanRings());

  float angle_min = 0., angle_max = 0.;

  for (int i = 0; i < cloudSize; i++) {

      /* 对点云进行坐标变换 */
      point.x = laserCloudIn->points[i].x; //laserCloudIn->points[i].y;
      point.y = laserCloudIn->points[i].y; //laserCloudIn->points[i].z;
      point.z = laserCloudIn->points[i].z; //laserCloudIn->points[i].x;
//      point.x = laserCloudIn[i].z;
//      point.y = laserCloudIn[i].y;
//      point.z = -1.0 * laserCloudIn[i].x;

    // skip NaN and INF valued points
    if (!pcl_isfinite(point.x) ||
        !pcl_isfinite(point.y) ||
        !pcl_isfinite(point.z)) {
      continue;
    }

    // skip zero valued points
    if (point.x * point.x + point.y * point.y + point.z * point.z < 0.0001) {
      continue;
    }

    // calculate vertical point angle and scan ID
    float angle = std::atan(point.y / std::sqrt(point.x * point.x + point.z * point.z));

    if(angle > angle_max) angle_max = angle;
    if(angle < angle_min) angle_min = angle;

    int scanID = _scanMapper.getRingForAngle(angle);

    if (scanID >= _scanMapper.getNumberOfScanRings() || scanID < 0 ){
      continue;
    }

    // calculate horizontal point angle
    float ori = -std::atan2(point.x, point.z);
    if (!halfPassed) {
      if (ori < startOri - M_PI / 2) {
        ori += 2 * M_PI;
      } else if (ori > startOri + M_PI * 3 / 2) {
        ori -= 2 * M_PI;
      }

      if (ori - startOri > M_PI) {
        halfPassed = true;
      }
    } else {
      ori += 2 * M_PI;

      if (ori < endOri - M_PI * 3 / 2) {
        ori += 2 * M_PI;
      } else if (ori > endOri + M_PI / 2) {
        ori -= 2 * M_PI;
      }
    }

    // calculate relative scan time based on point orientation
    float relTime = 0.1 * (ori - startOri) / (endOri - startOri); /* 激光传感器频率为10Hz */
//    point.intensity = (ori - startOri) / (endOri - startOri) * 256 - 1; /* 检测oritation计算是否正确 */
//    point.intensity = (scanID - 0.) / (64. - 0.) * 256 - 1; /* 检测scanID计算是否正确 */
    point.intensity = scanID + relTime;


//    projectPointToStartOfSweep(point, relTime);

    _laserCloudScans[scanID].push_back(point);
  }
//  std::cout << "angle_max = " << angle_max * 180. / M_PI << ", angle_min = " << angle_min * 180. / M_PI << std::endl;
//  std::cout << "## total size = " << cloudSize << std::endl;
//  for(int i = 0; i < 64; i++)
//      std::cout << "scanID " << i << ", " << "PtsNum " << _laserCloudScans[i].points.size() << std::endl;


    processScanlines(scanTime, _laserCloudScans, _cornerPointsSharp, _cornerPointsLessSharp, _surfPointsLessFlat, _surfPointsFlat);
  
}

} // end namespace loam
