#include <dual_quaternion.hpp>
#include <knn_point_cloud.hpp>
#include <kfusion/types.hpp>
#include <nanoflann.hpp>
#include "kfusion/warp_field.hpp"
#include "internal.hpp"
#include "precomp.hpp"
#include <opencv2/core/affine.hpp>
#define VOXEL_SIZE 100

using namespace kfusion;


typedef nanoflann::KDTreeSingleIndexAdaptor<
        nanoflann::L2_Simple_Adaptor<float, utils::PointCloud<float>>,
        utils::PointCloud<float>,
        3 /* dim */
> kd_tree_t;

WarpField::WarpField()
{}

WarpField::~WarpField()
{}
/**
 *
 * @param frame
 * \note The pose is assumed to be the identity, as this is the first frame
 */
void WarpField::init(const cuda::Cloud &frame){}
void WarpField::init(const std::vector<Vec3f> positions)
{
    nodes.reserve(positions.size());
    for(int i = 0; i < positions.size(); i++)
        nodes[i].vertex = positions[i];
}

void WarpField::energy(const cuda::Cloud &frame,
                       const cuda::Normals& normals,
                       const Affine3f &pose,
                       const cuda::TsdfVolume &tsdfVolume,
                       const std::vector<std::pair<utils::DualQuaternion<float>, utils::DualQuaternion<float>>> &edges
)
{
    assert(normals.cols()==frame.cols());
    assert(normals.rows()==frame.rows());

    int cols = frame.cols();

    std::vector<Point, std::allocator<Point>> cloud_host(size_t(frame.rows()*frame.cols()));
    frame.download(cloud_host, cols);

    std::vector<Normal, std::allocator<Normal>> normals_host(size_t(normals.rows()*normals.cols()));
    normals.download(normals_host, cols);
    for(size_t i = 0; i < cloud_host.size() && i < nodes.size(); i++) // FIXME: for now just stop at the number of nodes
    {
        auto point = cloud_host[i];
        auto norm = normals_host[i];
        if(!std::isnan(point.x))
        {
            // TODO:    transform by pose
            Vec3f position(point.x,point.y,point.z);
            Vec3f normal(norm.x,norm.y,norm.z);

            utils::DualQuaternion<float> dualQuaternion(utils::Quaternion<float>(0,position[0], position[1], position[2]),
                                                        utils::Quaternion<float>(normal));
            nodes[i].transform = dualQuaternion;
        }
        else
        {
            //    FIXME: will need to deal with the case when we get NANs
            std::cout<<"NANS"<<std::endl;
            break;
        }
    }


}

std::vector<node> WarpField::warp(std::vector<Vec3f> &frame) const
{
    std::vector<utils::DualQuaternion<float>> out_nodes(frame.size());
    for (auto vertex : frame)
    {
        utils::DualQuaternion<float> node = warp(vertex);
        out_nodes.push_back(node);
    }
    return nodes;
}

utils::DualQuaternion<float> kfusion::WarpField::warp(Vec3f point) const
{
    utils::DualQuaternion<float> out;
    utils::PointCloud<float> cloud;
    cloud.pts.resize(nodes.size());
    for(size_t i = 0; i < nodes.size(); i++)
    {
        utils::PointCloud<float>::Point point(nodes[i].transform.getTranslation().x_,
                                              nodes[i].transform.getTranslation().y_,
                                              nodes[i].transform.getTranslation().z_);
        cloud.pts[i] = point;
    }

    kd_tree_t index(3, cloud, nanoflann::KDTreeSingleIndexAdaptorParams(10));
    index.buildIndex();

    const size_t k = 8; //FIXME: number of neighbours should be a hyperparameter
    std::vector<utils::DualQuaternion<float>> neighbours(k);
    std::vector<size_t> ret_index(k);
    std::vector<float> out_dist_sqr(k);
    nanoflann::KNNResultSet<float> resultSet(k);
    resultSet.init(&ret_index[0], &out_dist_sqr[0]);

    index.findNeighbors(resultSet, point.val, nanoflann::SearchParams(10));

    for (size_t i = 0; i < k; i++)
        neighbours.push_back(nodes[ret_index[i]].transform);

    utils::DualQuaternion<float> node = DQB(point, VOXEL_SIZE);
    neighbours.clear();
    return node;
}

utils::DualQuaternion<float> WarpField::DQB(Vec3f vertex, float voxel_size) const
{
    utils::DualQuaternion<float> quaternion_sum;
    for(auto node : nodes)
    {
        utils::Quaternion<float> translation = node.transform.getTranslation();
        Vec3f voxel_center(translation.x_,translation.y_,translation.z_);
        quaternion_sum = quaternion_sum + weighting(vertex, voxel_center, voxel_size) * node.transform;
    }
    auto norm = quaternion_sum.magnitude();

    return utils::DualQuaternion<float>(quaternion_sum.getRotation() / norm.first,
                                        quaternion_sum.getTranslation() / norm.second);
}

//TODO: KNN already gives the squared distance as well, can pass here instead
float WarpField::weighting(Vec3f vertex, Vec3f voxel_center, float weight) const
{
    float diff = (float) cv::norm(voxel_center, vertex, cv::NORM_L2); // Should this be double?
    return exp(-(diff * diff) / (2 * weight * weight));
}