#include "cnn/model.h"
#include "cnn/tensor.h"
#include "cnn/aligned-mem-pool.h"
#include "cnn/cnn.h"
#include "cnn/macros.h"

#include <unordered_set>
#include <iostream>
#include <fstream>
#include <sstream>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>

#if HAVE_CUDA
#include <thrust/version.h>
#include "cnn/gpu-ops.h"
#include "cnn/cuda.h"
#include <thrust/device_ptr.h>
#include <thrust/transform.h>
#include <thrust/transform_reduce.h>
#include <cnn/functors.h>
#endif

using namespace std;

namespace cnn {

    extern AlignedMemoryPool<ALIGN>* glb_temp_lookup_gradient_value_mem;

    ParametersBase::~ParametersBase() {}

    Parameters::Parameters(const Dim& d, cnn::real scale, std::string nodename) : dim(d), name(nodename) {
        values.d = g.d = d;
        values.v = (cnn::real*)cnn_mm_malloc(d.size() * sizeof(cnn::real), CNN_ALIGN);
        values.m_device_id = device_id;
        if (scale == 1.0)
            /// fix scale to sqrt(6) / sqrt(d.d.sum_dims())
            TensorTools::Randomize(values);
        else
            TensorTools::Randomize(values, scale);
        g.v = (cnn::real*)cnn_mm_malloc(d.size() * sizeof(cnn::real), CNN_ALIGN);
        g.m_device_id = device_id;

        TensorTools::Zero(g);
    }

    size_t Parameters::size() const { return dim.size(); }

    void Parameters::reset_to_zero()
    {
#if HAVE_CUDA
        gpu::set_to_value_of(values.d.size(), values.v, 0.0);
#else
        (*values) *= 0.0;
#endif
    }

    void Parameters::scale_parameters(cnn::real a) {
        (*g) *= a;
    }

    void Parameters::squared_l2norm(cnn::real* sqnorm) const {
#if HAVE_CUDA
        gpu::l2_norm_reducer(values.d.size(), values.v, sqnorm, true, false);
#else
        *sqnorm = (*values).squaredNorm();
#endif
    }

    void Parameters::g_squared_l2norm(cnn::real* sqnorm) const {
#if HAVE_CUDA
        gpu::l2_norm_reducer(g.d.size(), g.v, sqnorm, true, false);
#else
        *sqnorm = (*g).squaredNorm();
#endif
    }

    /// use abs value to clip for each element
    /// compared to using gradient norm, this is simpler and cheaper
    void Parameters::g_simple_clipping(cnn::real threshold)  {
        cnn::real thr = threshold / g.d.size();
#if HAVE_CUDA
        gpu::simple_clipping(g.d.size(), g.v, g.v, thr);
#else
        for (int k = 0; k < g.d.size(); k++)
        {
            if (fabs(g.v[k]) > thr){
                if (g.v[k] > 0)
                    g.v[k] = thr;
                else
                    g.v[k] = -thr;
            }
        }
#endif
}

void Parameters::copy(const Parameters & param) {
  assert(dim == param.dim);
  TensorTools::CopyElements(values, param.values);
  this->name = param.name;
}

void Parameters::accumulate_grad(const Tensor& d) {
#if HAVE_CUDA
    if (sizeof(cnn::real) == sizeof(float))
        CUBLAS_CHECK(cublasSaxpy(cublas_handle, g.d.size(), reinterpret_cast<float*>(kSCALAR_ONE), reinterpret_cast<float*>(d.v), 1, reinterpret_cast<float*>(g.v), 1));
    else if (sizeof(cnn::real) == sizeof(double))
        CUBLAS_CHECK(cublasDaxpy(cublas_handle, g.d.size(), reinterpret_cast<double*>(kSCALAR_ONE), reinterpret_cast<double*>(d.v), 1, reinterpret_cast<double*>(g.v), 1));
#else
  *g += *d;
#endif
}

void Parameters::clear() {
  TensorTools::Zero(g);
}

LookupParameters::~LookupParameters()
{
    for (unsigned i = 0; i < values.size(); ++i) {
        auto& v = values[i];
#ifdef USE_CPU_FOR_LOOKUP_PARAM
        cnn_mm_free_host(v.v);
#else
        cnn_mm_free(v.v);
#endif
    }

    clear();
}

void LookupParameters::clear()
{
    /// the working memory is at GPU
    glb_temp_lookup_gradient_value_mem->free();

    values_for_non_zero_grads.clear();

    grads.clear();
}

LookupParameters::LookupParameters(unsigned n, const Dim& d, cnn::real scale, std::string nodename) : dim(d), values(n), grads(n), name(nodename) {
#ifdef USE_CPU_FOR_LOOKUP_PARAM
  bool b_cpu = true;
#else
  bool b_cpu = false; 
#endif

  for (unsigned i = 0; i < n; ++i) {
    auto& v = values[i];
    v.d = d;
#ifdef USE_CPU_FOR_LOOKUP_PARAM
    v.v = (cnn::real*)cnn_mm_malloc_host(d.size() * sizeof(cnn::real), CNN_ALIGN);
    v.m_device_id= CPUDEVICE; /// for cpu
#else
    v.v = (cnn::real*)cnn_mm_malloc(d.size() * sizeof(cnn::real), CNN_ALIGN);
    v.m_device_id = device_id;
#endif
	if (scale == 1.0)
		/// fix scale to sqrt(6) / sqrt(d.d.sum_dims())
		TensorTools::Randomize(v);
	else
		TensorTools::Randomize(v, scale);
  }
}

void LookupParameters::scale_parameters(cnn::real a) {
  for (auto& p : values)
    (*p) *= a;
}

void LookupParameters::Initialize(unsigned index, const vector<cnn::real>& val) {
  assert(int(val.size()) == int(dim.size()));
#if HAVE_CUDA
  cerr << "implement LookupParameters::Initialize\n";
  throw cuda_not_implemented("LookupParameters::Initialize");
#else
  memcpy(values[index].v, &val[0], val.size() * sizeof(cnn::real));
#endif
}

size_t LookupParameters::size() const {
  return values.size() * dim.size();
}

void LookupParameters::g_squared_l2norm(cnn::real* sqnorm) const {
#ifdef HAVE_CUDA
    bool acc = false;
    for (auto g : grads) {
        gpu::l2_norm_reducer(g.second.d.size(), g.second.v, sqnorm, true, acc);
        acc = true;
    }
#else
    real a = 0;
    for (auto g : grads)
    {
        a += (*g.second).squaredNorm();
    }
    *sqnorm = a;
#endif
}

void LookupParameters::squared_l2norm(cnn::real* sqnorm) const {
    cnn::real a = 0;
    for (unsigned i = 0; i < values.size(); ++i)
        a += (*values[i]).squaredNorm();
#if HAVE_CUDA
    CUDA_CHECK(cudaMemcpy(sqnorm, &a, sizeof(cnn::real), cudaMemcpyHostToDevice));
#else
    *sqnorm = a;
#endif
    /*
#if HAVE_CUDA
    bool acc = false;
    for (unsigned i = 0; i < values.size(); ++i) {
        gpu::l2_norm_reducer(values[i].d.size(), values[i].v, sqnorm, true, acc);
        acc = true;
    }
#else
    cnn::real a = 0;
    for (unsigned i = 0; i < values.size(); ++i)
        a += (*values[i]).squaredNorm();
    *sqnorm = a;
#endif
    */
}

void LookupParameters::g_simple_clipping(cnn::real thr)  {
#if HAVE_CUDA
    for (auto g : grads) {
        cnn::real threshold = thr / g.second.d.size();
        gpu::simple_clipping(g.second.d.size(), g.second.v, g.second.v, threshold);
    }
#else
    for (auto g: grads)
    {
        cnn::real threshold = thr / g.second.d.size();
        for (int k = 0; k < g.second.d.size(); k++)
        {
            if (fabs(g.second.v[k]) > threshold){
                if (g.second.v[k] > 0)
                    g.second.v[k] = threshold;
                else
                    g.second.v[k] = -threshold;
            }
        }
    }
#endif
}

void LookupParameters::copy(const LookupParameters & param) {
    assert(dim == param.dim);
    for (size_t i = 0; i < param.values.size(); ++i)
        TensorTools::CopyElements(values[i], param.values[i]);
    this->name = param.name;
}

void LookupParameters::copy(const std::map<int, std::vector<cnn::real>> & param) {
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (param.find(i) != param.end())
            TensorTools::SetElements(values[i], param.find(i)->second);
        else
            break;
    }
}

void LookupParameters::accumulate_grad(unsigned index, const Tensor& d) {
  if (grads.find(index) == grads.end())
  {
      cnn::real *g = (cnn::real*)glb_temp_lookup_gradient_value_mem->allocate(d.d.size() * sizeof(cnn::real));
      Tensor vv(d.d, g, device_id);
      vv.m_device_id = device_id; /// for cpu
      TensorTools::Zero(vv);  // gradient needs to be zero in the begining
      grads[index] = vv;
  }

#if HAVE_CUDA
  if (sizeof(cnn::real) == sizeof(float))
      CUBLAS_CHECK(cublasSaxpy(cublas_handle, d.d.size(), reinterpret_cast<float*>(kSCALAR_ONE), reinterpret_cast<float*>(d.v), 1, reinterpret_cast<float*>(grads[index].v), 1));
  else if (sizeof(cnn::real) == sizeof(double))
      CUBLAS_CHECK(cublasDaxpy(cublas_handle, d.d.size(), reinterpret_cast<double*>(kSCALAR_ONE), reinterpret_cast<double*>(d.v), 1, reinterpret_cast<double*>(grads[index].v), 1));
#else
  *grads[index] += *d;
#endif
}

Model::~Model() {
  for (auto p : all_params) delete p;
  if (gradient_norm_scratch)
      cnn_mm_free(gradient_norm_scratch); 
  if (gscale)
      cnn_mm_free_host(gscale);
}

void Model::project_weights(cnn::real radius) {
  static cnn::real* project_scratch = 0;
  if (!project_scratch)
    project_scratch = (cnn::real*)cnn_mm_malloc(all_params.size() * sizeof(cnn::real), CNN_ALIGN);
  int pi = 0;
  for (auto p : all_params) {
    p->squared_l2norm(&project_scratch[pi]);
    ++pi;
  }
  cnn::real gg = 0;
  for (int i = 0; i < pi; ++i)
    gg += project_scratch[i];
  cerr << "NORM: " << sqrt(gg) << endl;
}

cnn::real Model::gradient_l2_norm() const {
  if (!gradient_norm_scratch)
  {
      gradient_norm_scratch = (cnn::real*)cnn_mm_malloc(all_params.size() * sizeof(cnn::real), CNN_ALIGN);
  }
  if (gscale == nullptr)
      gscale = (cnn::real*)cnn_mm_malloc_host(sizeof(cnn::real) * all_params.size(), CNN_ALIGN);
  int pi = 0;
  for (auto p : all_params) {
    p->g_squared_l2norm(&gradient_norm_scratch[pi]);
    ++pi;
  }
#if HAVE_CUDA
//  gpu::l2_norm_reducer(all_params.size(), gradient_norm_scratch, gradient_norm_scratch, false, false);
//  cudaMemcpy(gscale, gradient_norm_scratch, sizeof(cnn::real),  cudaMemcpyDeviceToHost);
  // *gscale = sqrt(*gscale);
  // return *gscale;
  
  // do reduction in CPU as one whole block of memcpy can be fast
  CUDA_CHECK(cudaMemcpy(gscale, gradient_norm_scratch, sizeof(cnn::real) * all_params.size(), cudaMemcpyDeviceToHost));
  cnn::real gg = 0; 
  for (int k = 0; k < all_params.size(); k++)
      gg += gscale[k] * gscale[k];
  return sqrt(gg);
#else
  cnn::real gg = 0;
  for (int i = 0; i < pi; ++i)
    gg += gradient_norm_scratch[i];
  return sqrt(gg);
#endif
}

void Model::simple_gradient_clipping(cnn::real threshold)  {
    for (auto p : all_params) {
        p->g_simple_clipping(threshold);
    }
}

Parameters* Model::add_parameters(const Dim& d, cnn::real scale, std::string nodename) {
  Parameters* p = new Parameters(d, scale, nodename);
  all_params.push_back(p);
  params.push_back(p);
  return p;
}

LookupParameters* Model::add_lookup_parameters(unsigned n, const Dim& d, cnn::real scale, ::string nodename) {
  LookupParameters* p = new LookupParameters(n,d, scale, nodename);
  if (nodename != "") p->name = nodename;
  all_params.push_back(p);
  lookup_params.push_back(p);
  return p;
}

void Model::reset_gradient() {
  for (auto p : params) { p->clear(); }
  for (auto p : lookup_params) { p->clear(); }
}

void save_cnn_model(std::string filename, Model* model) {
#ifdef BINARY_BOOST
    ofstream out(filename, ios::binary);
    boost::archive::binary_oarchive oa(out);
#else
    ofstream out(filename, ofstream::out);
    boost::archive::text_oarchive oa(out);
#endif
    oa << *model;
    out.close();
};

void load_cnn_model(std::string filename, Model* model) {
#ifdef BINARY_BOOST
    ifstream in(filename, ios::binary);
    if (in.is_open())
    {
        boost::archive::binary_iarchive ia(in);
#else
    ifstream in(filename, ifstream::in);
    if (in.is_open())
    {
        boost::archive::text_iarchive ia(in);
#endif
        ia >> *model;
    }
};


} // namespace cnn
