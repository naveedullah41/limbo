#ifndef LIMBO_EXPERIMENTAL_MODEL_SSPGR_HPP
#define LIMBO_EXPERIMENTAL_MODEL_SSPGR_HPP

#include <vector>

#include <Eigen/Cholesky>
#include <Eigen/Core>
#include <Eigen/Dense>
#include <Eigen/LU>

#include <limbo/model/gp/no_lf_opt.hpp>

namespace limbo {
    namespace experimental {
        namespace model {

            template <typename Params, typename Mapping, typename MeanFunction, class HyperParamsOptimizer = limbo::model::gp::NoLFOpt<Params>>
            class SSGPR {
            public:
                SSGPR() : _dim_in(-1), _dim_out(-1), _sigma_n(0.1) {}

                SSGPR(int dim_in, int dim_out, double sigma_n = 0.1) : _dim_in(dim_in), _dim_out(dim_out), _sigma_n(sigma_n), _mapping(dim_in), _mean_function(dim_out)
                {
                    reset();
                }

                void reset()
                {
                    assert(_dim_in > 0 && _dim_out > 0);

                    int n = _mapping.dim_out();
                    _L = _sigma_n * _sigma_n * Eigen::MatrixXd::Identity(n, n);
                    _B = Eigen::MatrixXd::Zero(n, _dim_out);
                    _W = Eigen::MatrixXd::Zero(n, _dim_out);
                }

                std::tuple<Eigen::VectorXd, double> predict(const Eigen::VectorXd& x) const
                {
                    double sigma_n_2 = _sigma_n * _sigma_n;

                    if (_samples.size() == 0)
                        return std::make_tuple(_mean_function(x, *this), sigma_n_2);

                    Eigen::VectorXd PhiStar = _mapping.evaluate(x);

                    // std::cout << PhiStar.rows() << "x" << PhiStar.cols() << " " << _W.rows() << "x" << _W.cols() << std::endl;
                    Eigen::VectorXd mean = PhiStar.transpose() * _W;

                    // std::cout << _L.rows() << "x" << _L.cols() << " " << PhiStar.rows() << "x" << PhiStar.cols() << std::endl;
                    Eigen::VectorXd sol = _L.template triangularView<Eigen::Lower>().solve(PhiStar);

                    // Eigen::VectorXd var(_p);
                    // Eigen::VectorXd tmp = sol; //sol.array().square().colwise().sum();
                    // std::cout << tmp.rows() << "x" << tmp.cols() << std::endl;
                    double var = sigma_n_2 * (1. + sol.array().square().sum());

                    return std::make_tuple(mean + _mean_function(x, *this), var);
                }

                void compute(const std::vector<Eigen::VectorXd>& samples, const std::vector<Eigen::VectorXd>& observations)
                {
                    assert(samples.size() != 0);
                    assert(observations.size() != 0);
                    assert(samples.size() == observations.size());

                    if ((_dim_in != samples[0].size())) {
                        _dim_in = samples[0].size();
                        _mapping = Mapping(_dim_in); // rebuilding a mapping is necessary
                        reset();
                    }

                    if (_dim_out != observations[0].size()) {
                        _dim_out = observations[0].size();
                        _mean_function = MeanFunction(_dim_out); // the cost of building a functor should be relatively low
                        reset();
                    }

                    _samples = samples;

                    _observations.resize(observations.size(), _dim_out);
                    for (int i = 0; i < observations.size(); ++i)
                        _observations.row(i) = observations[i];

                    // _mean_observation = _observations.colwise().mean();

                    this->_compute_obs_mean();
                    this->_full_compute();
                }

                /// Do not forget to call this if you use hyper-prameters optimization!!
                void optimize_hyperparams()
                {
                    _hp_optimize(*this);
                }

                ///  recomputes the GP
                void recompute(bool update_obs_mean = true)
                {
                    assert(!_samples.empty());

                    if (update_obs_mean)
                        this->_compute_obs_mean();

                    this->_full_compute();
                }

                double compute_log_lik()
                {
                    assert(_samples.size() > 0);

                    Eigen::MatrixXd tmp = (_obs_mean.transpose() * _obs_mean);
                    double y_T_y = tmp(0, 0);
                    tmp = _obs_mean.transpose() * _Phi * _W;
                    double big = tmp(0, 0);

                    double sigma_n_2 = _sigma_n * _sigma_n;

                    _log_lik = -0.5 * ((y_T_y - big) / sigma_n_2 - _mapping.dim_out() * std::log(sigma_n_2) + _samples.size() * std::log(2. * M_PI * sigma_n_2)) - _L.diagonal().array().log().sum();

                    return _log_lik;
                }

                /// return the likelihood (do not compute it -- return last computed)
                double get_log_lik() const { return _log_lik; }

                /// set the log likelihood (e.g. computed from outside)
                void set_log_lik(double log_lik) { _log_lik = log_lik; }

                Eigen::VectorXd params() const
                {
                    assert(_dim_in > 0 && _dim_out > 0);

                    Eigen::VectorXd mapping_params = _mapping.params();
                    mapping_params.head(1 + _dim_in) = mapping_params.head(1 + _dim_in).array().log();

                    int m_size = mapping_params.size();

                    Eigen::VectorXd p(m_size + 1);
                    p(0) = std::log(_sigma_n); //_sigma_n; //std::log(_sigma_n);
                    p.tail(m_size) = mapping_params;

                    return p;
                }

                void set_params(const Eigen::VectorXd& p)
                {
                    assert(_dim_in > 0 && _dim_out > 0);

                    int m_size = _mapping.params().size();

                    Eigen::VectorXd pp = p.tail(m_size);
                    pp.head(1 + _dim_in) = pp.head(1 + _dim_in).array().exp();

                    _mapping.set_params(pp);
                    _sigma_n = std::exp(p(0)); //p(0); //std::exp(p(0)) + 1e-6;
                }

            protected:
                int _dim_in, _dim_out;
                double _sigma_n;
                Mapping _mapping;
                MeanFunction _mean_function;

                Eigen::MatrixXd _L, _B, _W;

                Eigen::MatrixXd _observations;
                Eigen::MatrixXd _mean_vector;
                Eigen::MatrixXd _obs_mean;
                std::vector<Eigen::VectorXd> _samples;

                // Eigen::VectorXd _mean_observation;

                Eigen::MatrixXd _Phi, _LPhiY;

                HyperParamsOptimizer _hp_optimize;

                double _log_lik;

                void _compute_obs_mean()
                {
                    _mean_vector.resize(_samples.size(), _dim_out);
                    for (int i = 0; i < _mean_vector.rows(); i++)
                        _mean_vector.row(i) = _mean_function(_samples[i], *this);
                    _obs_mean = _observations - _mean_vector;
                }

                void _full_compute()
                {
                    double sigma_n_2 = _sigma_n * _sigma_n;

                    _Phi = Eigen::MatrixXd::Zero(_samples.size(), _mapping.dim_out());
                    // TO-DO: Maybe we can vectorize this for faster computation
                    for (size_t i = 0; i < _samples.size(); i++) {
                        _Phi.row(i) = _mapping.evaluate(_samples[i]);
                    }

                    _L = _Phi.transpose() * _Phi;
                    _L.diagonal().array() += sigma_n_2;

                    Eigen::LLT<Eigen::MatrixXd> llt(_L);
                    if (llt.info() == Eigen::NumericalIssue) {
                        // std::cout << "Error" << std::endl;
                        // there was an error
                        // probably the matrix is not SPD
                        // let's try to make it
                        // original MATLAB code: http://fr.mathworks.com/matlabcentral/fileexchange/42885-nearestspd
                        Eigen::MatrixXd B = (_L.array() + _L.transpose().array()) / 2.;

                        Eigen::JacobiSVD<Eigen::MatrixXd> svd(B, Eigen::ComputeFullU | Eigen::ComputeFullV);
                        Eigen::MatrixXd V, Sigma, H, L_hat;

                        Sigma = Eigen::MatrixXd::Identity(B.rows(), B.cols());
                        Sigma.diagonal() = svd.singularValues();
                        V = svd.matrixV();

                        H = V * Sigma * V.transpose();

                        L_hat = (B.array() + H.array()) / 2.;
                        L_hat = (L_hat.array() + L_hat.array()) / 2.;

                        Eigen::LLT<Eigen::MatrixXd> llt_hat(L_hat);
                        int k = 0;
                        while (llt_hat.info() != Eigen::Success) {
                            k++;
                            Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(L_hat);
                            double min_eig = es.eigenvalues().minCoeff();
                            L_hat.diagonal().array() += (-min_eig * k * k + 1e-50);

                            llt_hat.compute(L_hat);
                        }
                        _L = llt_hat.matrixL();
                        // if (llt_hat.info() == Eigen::Success)
                        //     std::cout << "Fixed!" << std::endl;
                    }
                    else
                        _L = llt.matrixL();

                    _B = _Phi.transpose() * _obs_mean;

                    // std::cout << _B << std::endl;

                    _LPhiY = _L.template triangularView<Eigen::Lower>().solve(_B);
                    _W = _L.template triangularView<Eigen::Lower>().adjoint().solve(_LPhiY);
                }
            };
        }
    }
}

#endif