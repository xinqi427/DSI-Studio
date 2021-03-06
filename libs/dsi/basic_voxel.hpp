#ifndef BASIC_VOXEL_HPP
#define BASIC_VOXEL_HPP
#include <boost/mpl/vector.hpp>
#include <boost/mpl/for_each.hpp>
#include <boost/mpl/inherit_linearly.hpp>
#include <image/image.hpp>
#include <string>
#include "tessellated_icosahedron.hpp"
#include "gzip_interface.hpp"
#include "prog_interface_static_link.h"
struct ImageModel;
struct VoxelParam;
class Voxel;
struct VoxelData;
class BaseProcess
{
public:
    BaseProcess(void) {}
    virtual void init(Voxel&) {}
    virtual void run(Voxel&, VoxelData&) {}
    virtual void end(Voxel&,gz_mat_write&) {}
    virtual ~BaseProcess(void) {}
};



struct VoxelData
{
    unsigned int voxel_index;
    std::vector<float> space;
    std::vector<float> odf;
    std::vector<float> fa;
    std::vector<float> rdi;
    std::vector<image::vector<3,float> > dir;
    std::vector<short> dir_index;
    float min_odf;
    float jdet;
    image::matrix<3,3,float> jacobian;

    void init(void)
    {
        std::fill(fa.begin(),fa.end(),0.0);
        std::fill(dir_index.begin(),dir_index.end(),0);
        std::fill(dir.begin(),dir.end(),image::vector<3,float>());
        min_odf = 0.0;
    }
};

class Voxel
{
private:
    std::vector<std::shared_ptr<BaseProcess> > process_list;
public:
    image::geometry<3> dim;
    image::vector<3> vs;
    std::vector<image::vector<3,float> > bvectors;
    std::vector<float> bvalues;
    image::basic_image<float,3> dwi_sum;
    std::string report;
    std::ostringstream recon_report;
    unsigned int thread_count = 1;
public:// parameters;
    tessellated_icosahedron ti;
    const float* param;
    std::string file_name;
    bool need_odf = false;
    bool half_sphere = false;
    unsigned int max_fiber_number = 5;
    std::vector<std::string> file_list;
public:// DTI
    bool output_diffusivity = false;
    bool output_tensor = false;
public://used in GQI
    bool r2_weighted = false;// used in GQI only
    bool scheme_balance = false;
    bool csf_calibration = false;
public:// odf sharpening
    bool odf_deconvolusion = false;
    bool odf_decomposition = false;
    image::vector<3,short> odf_xyz;
public:// gradient deviation
    std::vector<image::basic_image<float,3> > new_grad_dev;
    std::vector<image::pointer_image<float,3> > grad_dev;
public:// used in QSDR
    unsigned char reg_method = 0;
    image::transformation_matrix<double> qsdr_trans;
    bool output_jacobian = false;
    bool output_mapping = false;
    bool output_rdi = false;
    image::vector<3,int> csf_pos1,csf_pos2,csf_pos3,csf_pos4;
    double R2;
public: // for QSDR associated T1WT2W
    std::vector<image::basic_image<float,3> > other_image;
    std::vector<std::string> other_image_name;
    std::vector<image::transformation_matrix<double> > other_image_affine;
public: // for T1W based DMDM
    image::basic_image<float,3> t1w,t1wt;
    image::vector<3> t1w_vs,t1wt_vs;
    float t1wt_tran[16];
    std::string t1w_file_name;

public: // user in fib evaluation
    std::vector<float> fib_fa;
    std::vector<float> fib_dir;
public:
    float z0 = 0.0;
    // other information for second pass processing
    std::vector<float> response_function,free_water_diffusion;
    image::basic_image<float,3> qa_map;
    float reponse_function_scaling;
public:// for template creation
    std::vector<std::vector<float> > template_odfs;
    std::string template_file_name;
public:
    std::vector<VoxelData> voxel_data;
public:
    ImageModel* image_model;
public:
    template<class ProcessList>
    void CreateProcesses(void)
    {
        process_list.clear();
        boost::mpl::for_each<ProcessList>(boost::ref(*this));
    }

    template<class Process>
    void operator()(Process&)
    {
        process_list.push_back(std::make_shared<Process>());
    }
public:
    void init(void)
    {
        voxel_data.resize(thread_count);
        for (unsigned int index = 0; index < thread_count; ++index)
        {
            voxel_data[index].space.resize(bvalues.size());
            voxel_data[index].odf.resize(ti.half_vertices_count);
            voxel_data[index].fa.resize(max_fiber_number);
            voxel_data[index].dir_index.resize(max_fiber_number);
            voxel_data[index].dir.resize(max_fiber_number);
        }
        for (unsigned int index = 0; index < process_list.size(); ++index)
            process_list[index]->init(*this);
    }

    void run(const image::basic_image<unsigned char,3>& mask)
    {
        try{

        size_t total_voxel = 0;
        bool terminated = false;
        begin_prog("reconstructing");
        for(size_t index = 0;index < mask.size();++index)
            if (mask[index])
                ++total_voxel;

        unsigned int total = 0;
        image::par_for2(mask.size(),
                        [&](int voxel_index,int thread_index)
        {
            ++total;
            if(terminated || !mask[voxel_index])
                return;
            if(thread_index == 0)
            {
                if(prog_aborted())
                {
                    terminated = true;
                    return;
                }
                check_prog(total,mask.size());
            }
            voxel_data[thread_index].init();
            voxel_data[thread_index].voxel_index = voxel_index;
            for (int index = 0; index < process_list.size(); ++index)
                process_list[index]->run(*this,voxel_data[thread_index]);
        },thread_count);
        check_prog(1,1);
        }
        catch(std::exception& error)
        {
            std::cout << error.what() << std::endl;
        }
        catch(...)
        {
            std::cout << "unknown error" << std::endl;
        }

    }

    void end(gz_mat_write& writer)
    {
        begin_prog("output data");
        for (unsigned int index = 0; check_prog(index,process_list.size()); ++index)
            process_list[index]->end(*this,writer);
    }

    BaseProcess* get(unsigned int index)
    {
        return process_list[index].get();
    }
};

struct terminated_class {
    unsigned int total;
    mutable unsigned int now;
    mutable bool terminated;
    terminated_class(int total_):total(total_),now(0),terminated(false){}
    bool operator!() const
    {
        terminated = prog_aborted();
        return check_prog(std::min(now++,total-1),total);
    }
    ~terminated_class()
    {
        check_prog(total,total);
    }
};



#endif//BASIC_VOXEL_HPP
