/* ===========================
 *
 * Copyright (c) 2013 Philippe Tillet - National Chiao Tung University
 *
 * CLICA - Hybrid ICA using ViennaCL + Eigen
 *
 * License : MIT X11 - See the LICENSE file in the root folder
 * ===========================*/

#include "tests/benchmark-utils.hpp"

#include "fmincl/check_grad.hpp"
#include "fmincl/minimize.hpp"

#include "src/whiten.hpp"
#include "src/utils.hpp"
#include "src/backend.hpp"


#include "fastapprox-0/fasthyperbolic.h"
#include "fastapprox-0/fastlog.h"

#include <pmmintrin.h>

namespace parica{


template<class ScalarType>
struct ica_functor{
private:

    static const int alpha_sub = 4;
    static const int alpha_gauss = 2;
    static const int alpha_super = 1;
private:
    template <typename T>
    inline int sgn(T val) const {
        return (val>0)?1:-1;
    }

    inline void fill_y() const;
    inline void compute_means_logp() const;

public:
    ica_functor(ScalarType const * data, std::size_t NF, std::size_t NC, std::size_t BNF) : data_(data), NC_(NC), NF_(NF), BNF_(BNF){
        ipiv_ =  new typename backend<ScalarType>::size_t[NC_+1];

        subdata = new ScalarType[NC_*BNF_];

        z1 = new ScalarType[NC_*BNF_];
        y_ = new float[NC_*BNF_];

        phi = new ScalarType[NC_*BNF_];


        phi_z1t = new ScalarType[NC_*NC_];
        dweights = new ScalarType[NC_*NC_];
        dbias = new ScalarType[NC_];
        W = new ScalarType[NC_*NC_];
        WLU = new ScalarType[NC_*NC_];
        b_ = new ScalarType[NC_];
        kurt = new ScalarType[NC_];
        means_logp = new ScalarType[NC_];
    }

    ~ica_functor(){
        delete[] ipiv_;

        delete[] z1;
        delete[] phi;
        delete[] phi_z1t;
        delete[] dweights;
        delete[] dbias;
        delete[] W;
        delete[] WLU;
        delete[] b_;
        delete[] kurt;
        delete[] means_logp;
    }

    void new_minibatch_callback(std::size_t i) const{
        k_ = i;
    }

    void operator()(ScalarType const * x, ScalarType* value, ScalarType ** grad) const {
        Timer t;
        t.start();

        //Rerolls the variables into the appropriates datastructures
        std::memcpy(W, x,sizeof(ScalarType)*NC_*NC_);
        std::memcpy(b_, x+NC_*NC_, sizeof(ScalarType)*NC_);


        std::size_t TBNF = std::min(BNF_, NF_ - k_*BNF_);

        //z1 = W*data_;
        backend<ScalarType>::gemm(NoTrans,NoTrans,TBNF,NC_,NC_,1,data_+k_*BNF_,NF_,W,NC_,0,z1,BNF_);

        for(unsigned int c = 0 ; c < NC_ ; ++c){
            ScalarType m2 = 0, m4 = 0;
            ScalarType b = b_[c];
            for(unsigned int f = 0; f < TBNF ; f++){
                ScalarType X = z1[c*BNF_+f] + b;
                m2 += std::pow(X,2);
                m4 += std::pow(X,4);
            }
            m2 = std::pow(1/(ScalarType)TBNF*m2,2);
            m4 = 1/(ScalarType)TBNF*m4;
            ScalarType k = m4/m2 - 3;
            kurt[c] = k+0.02;
        }

        fill_y();

        compute_means_logp();

        //H = log(abs(det(w))) + sum(means_logp);
        //LU Decomposition
        std::memcpy(WLU,W,sizeof(ScalarType)*NC_*NC_);
        backend<ScalarType>::getrf(NC_,NC_,WLU,NC_,ipiv_);
        //det = prod(diag(WLU))
        ScalarType absdet = 1;
        for(std::size_t i = 0 ; i < NC_ ; ++i)
            absdet*=std::abs(WLU[i*NC_+i]);
        ScalarType H = std::log(absdet);
        for(std::size_t i = 0; i < NC_ ; ++i)
            H+=means_logp[i];

        if(value){
            *value = -H;
        }

        if(grad){
            //phi = mean(mata.*abs(z2).^(mata-1).*sign(z2),2);
            for(unsigned int c = 0 ; c < NC_ ; ++c){
                ScalarType k = kurt[c];
                ScalarType b = b_[c];
                for(unsigned int f = 0 ; f < TBNF ; f++){
                    ScalarType z2 = z1[c*BNF_+f] + b;
                    ScalarType y = y_[c*BNF_+f];
                    phi[c*BNF_+f] =(k<0)?(z2 - y):(z2 + 2*y);
                }
            }


            //dbias = mean(phi,2)
            for(std::size_t c = 0 ; c < NC_ ;++c){
                ScalarType sum = 0;
                for(std::size_t f = 0 ; f < TBNF ; ++f)
                    sum += phi[c*BNF_+f];
                dbias[c] = sum/(ScalarType)TBNF;
            }


            /*dweights = -(eye(N) - 1/n*phi*z1')*inv(W)'*/
            //WLU = inv(W)
            backend<ScalarType>::getri(NC_,WLU,NC_,ipiv_);
            //lhs = I(N,N) - 1/N*phi*z1')
            backend<ScalarType>::gemm(Trans,NoTrans,NC_,NC_,TBNF ,-1/(ScalarType)BNF_,z1,BNF_,phi,BNF_,0,phi_z1t,NC_);
            for(std::size_t i = 0 ; i < NC_; ++i)
                phi_z1t[i*NC_+i] += 1;
            //dweights = -lhs*Winv'
            backend<ScalarType>::gemm(Trans,NoTrans,NC_,NC_,NC_,-1,WLU,NC_,phi_z1t,NC_,0,dweights,NC_);
            //Copy back
            std::memcpy(*grad, dweights,sizeof(ScalarType)*NC_*NC_);
            std::memcpy(*grad+NC_*NC_, dbias, sizeof(ScalarType)*NC_);
        }

    }

private:
    ScalarType const * data_;
    std::size_t NC_;
    std::size_t NF_;

    mutable std::size_t k_;
    std::size_t BNF_;


    typename backend<ScalarType>::size_t *ipiv_;

    ScalarType* subdata;

    ScalarType* z1;
    ScalarType* phi;

    float* y_;

    //Mixing
    ScalarType* phi_z1t;
    ScalarType* dweights;
    ScalarType* dbias;
    ScalarType* W;
    ScalarType* WLU;
    ScalarType* b_;
    ScalarType* kurt;
    ScalarType* means_logp;

};

template<>
void ica_functor<float>::fill_y() const{
    std::size_t TBNF = std::min(BNF_, NF_ - k_*BNF_);
    for(unsigned int c = 0 ; c < NC_ ; ++c){
        const __m128 bias = _mm_set1_ps(b_[c]);
        for(unsigned int f = 0; f < TBNF ; f+=4){
            __m128 z2 = _mm_load_ps(&z1[c*BNF_+f]);
            z2 = _mm_add_ps(z2,bias);
            __m128 y = vfasttanh(z2);
            _mm_store_ps(&y_[c*BNF_+f],y);
        }
    }
}

template<>
void ica_functor<double>::fill_y() const{
    std::size_t TBNF = std::min(BNF_, NF_ - k_*BNF_);
    for(unsigned int c = 0 ; c < NC_ ; ++c){
        float fbias = b_[c];
        const __m128 bias = _mm_set1_ps(fbias);
        for(unsigned int f = 0; f < TBNF ; f+=4){
            __m128d z2lo = _mm_load_pd(&z1[c*BNF_+f]);
            __m128d z2hi = _mm_load_pd(&z1[c*BNF_+f+2]);
            __m128 z2 = _mm_movelh_ps(_mm_cvtpd_ps(z2lo), _mm_cvtpd_ps(z2hi));
            z2 = _mm_add_ps(z2,bias);
            __m128 y = vfasttanh(z2);
            _mm_store_ps(&y_[c*BNF_+f],y);
        }
    }
}

template<>
void ica_functor<float>::compute_means_logp() const{
    std::size_t TBNF = std::min(BNF_, NF_ - k_*BNF_);
    for(unsigned int c = 0 ; c < NC_ ; ++c){
        __m128d vsum = _mm_set1_pd(0.0d);
        float k = kurt[c];
        const __m128 bias = _mm_set1_ps(b_[c]);
        for(unsigned int f = 0; f < TBNF ; f+=4){
            __m128 z2 = _mm_load_ps(&z1[c*BNF_+f]);
            z2 = _mm_add_ps(z2,bias);
            const __m128 _1 = _mm_set1_ps(1);
            const __m128 m0_5 = _mm_set1_ps(-0.5);
            if(k<0){
                const __m128 vln0_5 = _mm_set1_ps(-0.693147);

                __m128 a = _mm_sub_ps(z2,_1);
                a = _mm_mul_ps(a,a);
                a = _mm_mul_ps(m0_5,a);
                a = vfastexp(a);

                __m128 b = _mm_add_ps(z2,_1);
                b = _mm_mul_ps(b,b);
                b = _mm_mul_ps(m0_5,b);
                b = vfastexp(b);

                a = _mm_add_ps(a,b);
                a = vfastlog(a);

                a = _mm_add_ps(vln0_5,a);

                vsum=_mm_add_pd(vsum,_mm_cvtps_pd(a));
                vsum=_mm_add_pd(vsum,_mm_cvtps_pd(_mm_movehl_ps(a,a)));
            }
            else{
                __m128 z22 = _mm_mul_ps(z2,z2);
                z22 = _mm_mul_ps(_mm_set1_ps(0.5),z22);

                __m128 y = _mm_load_ps(&y_[c*BNF_+f]);
                y = _mm_mul_ps(y,y);
                y = _mm_sub_ps(_1,y);
                y = vfastlog(y);
                y = _mm_sub_ps(y,z22);

                vsum=_mm_add_pd(vsum,_mm_cvtps_pd(y));
                vsum=_mm_add_pd(vsum,_mm_cvtps_pd(_mm_movehl_ps(y,y)));
            }
        }
        double sum;
        vsum = _mm_hadd_pd(vsum, vsum);
        _mm_store_sd(&sum, vsum);
        means_logp[c] = 1/(double)TBNF*sum;
    }
}

template<>
void ica_functor<double>::compute_means_logp() const{
    std::size_t TBNF = std::min(BNF_, NF_ - k_*BNF_);
    for(unsigned int c = 0 ; c < NC_ ; ++c){
        __m128d vsum = _mm_set1_pd(0.0d);
        float k = kurt[c];
        const __m128 bias = _mm_set1_ps(b_[c]);
        for(unsigned int f = 0; f < TBNF ; f+=4){
            __m128d z2lo = _mm_load_pd(&z1[c*BNF_+f]);
            __m128d z2hi = _mm_load_pd(&z1[c*BNF_+f+2]);
            __m128 z2 = _mm_movelh_ps(_mm_cvtpd_ps(z2lo), _mm_cvtpd_ps(z2hi));
            z2 = _mm_add_ps(z2,bias);
            const __m128 _1 = _mm_set1_ps(1);
            const __m128 m0_5 = _mm_set1_ps(-0.5);
            if(k<0){
                const __m128 vln0_5 = _mm_set1_ps(-0.693147);

                __m128 a = _mm_sub_ps(z2,_1);
                a = _mm_mul_ps(a,a);
                a = _mm_mul_ps(m0_5,a);
                a = vfastexp(a);

                __m128 b = _mm_add_ps(z2,_1);
                b = _mm_mul_ps(b,b);
                b = _mm_mul_ps(m0_5,b);
                b = vfastexp(b);

                a = _mm_add_ps(a,b);
                a = vfastlog(a);

                a = _mm_add_ps(vln0_5,a);

                vsum=_mm_add_pd(vsum,_mm_cvtps_pd(a));
                vsum=_mm_add_pd(vsum,_mm_cvtps_pd(_mm_movehl_ps(a,a)));
            }
            else{
                __m128 z22 = _mm_mul_ps(z2,z2);
                z22 = _mm_mul_ps(_mm_set1_ps(0.5),z22);

                __m128 y = _mm_load_ps(&y_[c*BNF_+f]);
                y = _mm_mul_ps(y,y);
                y = _mm_sub_ps(_1,y);
                y = vfastlog(y);
                y = _mm_sub_ps(y,z22);

                vsum=_mm_add_pd(vsum,_mm_cvtps_pd(y));
                vsum=_mm_add_pd(vsum,_mm_cvtps_pd(_mm_movehl_ps(y,y)));
            }
        }
        double sum;
        vsum = _mm_hadd_pd(vsum, vsum);
        _mm_store_sd(&sum, vsum);
        means_logp[c] = 1/(double)TBNF*sum;
    }
}



fmincl::optimization_options make_default_options(){
    fmincl::optimization_options options;
    options.direction = new fmincl::quasi_newton();
    options.max_iter = 200;
    options.verbosity_level = 2;
    options.line_search = new fmincl::strong_wolfe_powell(5);
    options.stopping_criterion = new fmincl::gradient_treshold(1e-6);
    return options;
}


template<class ScalarType>
void inplace_linear_ica(ScalarType const * data, ScalarType * out, std::size_t NC, std::size_t NF, fmincl::optimization_options const & options){
    typedef typename fmincl_backend<ScalarType>::type BackendType;

    std::size_t N = NC*NC + NC;

    ScalarType * Sphere = new ScalarType[NC*NC];
    ScalarType * W = new ScalarType[NC*NC];
    ScalarType * b = new ScalarType[NC];
    ScalarType * X = new ScalarType[N];
    std::memset(X,0,N*sizeof(ScalarType));
    ScalarType * white_data = new ScalarType[NC*NF];

    //Optimization Vector

    //Solution vector
    //Initial guess W_0 = I
    //b_0 = 0
    for(unsigned int i = 0 ; i < NC; ++i)
        X[i*(NC+1)] = 1;

    //Whiten Data
    whiten<ScalarType>(NC, NF, data,Sphere);

    //white_data = randperm(2*Sphere*data)
    backend<ScalarType>::gemm(NoTrans,NoTrans,NF,NC,NC,2,data,NF,Sphere,NC,0,white_data,NF);

    detail::shuffle(white_data,NC,NF);


    std::size_t BNF = 40000;
    std::size_t num_batches = (NF%BNF==0)?NF/BNF:NF/BNF+1;
    ica_functor<ScalarType> objective(white_data,NF,NC,BNF);
    fmincl::optimization_options optimization_options;
    optimization_options.direction = options.direction;
    optimization_options.max_iter = 3;
    optimization_options.verbosity_level = 0;
    ScalarType * tmp = new ScalarType[N];
    for(std::size_t epoch = 0 ; epoch < options.max_iter ; ++ epoch){
        for(std::size_t batch = 0 ; batch < num_batches; ++batch){
            objective.new_minibatch_callback(batch);
            fmincl::optimization_result res = fmincl::minimize<BackendType>(X,objective,X,N,optimization_options);
            if(batch==0)
                std::cout << epoch << " : " << res.f << std::endl;
        }
    }

    //Copies into datastructures
    std::memcpy(W, X,sizeof(ScalarType)*NC*NC);
    std::memcpy(b, X+NC*NC, sizeof(ScalarType)*NC);

    //out = W*Sphere*data;
    backend<ScalarType>::gemm(NoTrans,NoTrans,NF,NC,NC,2,data,NF,Sphere,NC,0,white_data,NF);
    backend<ScalarType>::gemm(NoTrans,NoTrans,NF,NC,NC,1,white_data,NF,W,NC,0,out,NF);

    for(std::size_t c = 0 ; c < NC ; ++c){
        ScalarType val = b[c];
        for(std::size_t f = 0 ; f < NF ; ++f){
            out[c*NF+f] += val;
        }
    }



    delete[] W;
    delete[] b;
    delete[] X;
    delete[] white_data;

}

template void inplace_linear_ica<float>(float const * data, float * out, std::size_t NC, std::size_t NF, fmincl::optimization_options const & options);
template void inplace_linear_ica<double>(double const * data, double * out, std::size_t NC, std::size_t NF, fmincl::optimization_options const & options);

}

