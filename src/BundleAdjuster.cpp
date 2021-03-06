#include <ba/BundleAdjuster.h>
#include <iomanip>
#include <fstream>
#include <ba/parallel_algos.h>
#include <xmmintrin.h>


namespace ba {
  // these are declared in Utils.h
  int debug_level = 0;
  int debug_level_threshold = 0;
  // #define DAMPING 0.1

  // Definition of static vars.
  template<typename Scalar,int LmSize, int PoseSize, int CalibSize, bool DoTvs>
  constexpr uint32_t BundleAdjuster<Scalar, LmSize, PoseSize,
  CalibSize, DoTvs>::kCalibDim;

  ////////////////////////////////////////////////////////////////////////////////
  template<typename Scalar,int LmSize, int PoseSize, int CalibSize, bool DoTvs>
  void BundleAdjuster<Scalar, LmSize, PoseSize, CalibSize, DoTvs>::ApplyUpdate(
      const Delta& delta, const bool do_rollback,
      const Scalar damping)
  {
    // Write the delta norm into the solution summary.
    summary_.delta_norm = delta.delta_l.norm() + delta.delta_p.norm();

    Eigen::Matrix<Scalar, 3, 3> j_pose_update;

    Scalar coef = (do_rollback == true ? -1.0 : 1.0) * damping;
    // update gravity terms if necessary
    if (inertial_residuals_.size() > 0) {
      const VectorXt delta_calib = delta.delta_p.template tail(kCalibDim)*coef;
      if (kGravityInCalib) {
        imu_.g -= delta_calib.template head<2>();

        StreamMessage(debug_level) << "Gravity delta is " <<
                                      delta_calib.template head<2>().transpose() << " gravity is: " <<

                                      imu_.g.transpose() << std::endl;
        imu_.g_vec = GetGravityVector(imu_.g);
      }
    }

  // Update the camera intrinsics if optimized.
  if (kCamParamsInCalib && delta.delta_k.rows() > 0){
    Eigen::VectorXd params = rig_->cameras_[0]->GetParams();
    StreamMessage(debug_level) << "Prev params: " << params.transpose() <<
                                  std::endl;

    // Apply the update (camera intrinsics will always be first).
    params -= delta.delta_k.template head<CalibSize>();
    rig_->cameras_[0]->SetParams(params);

    StreamMessage(debug_level) << " Post params: " << params.transpose() <<
                                    std::endl;

    // If we are in inverse depth mode, we have to reproject all landmarks.
    if (kLmDim == 1) {
      for (size_t ii = 0 ; ii < landmarks_.size() ; ++ii) {
        Landmark& lm = landmarks_[ii];
        // std::cerr << "Prev x_s for lm " << ii << ":" << lm.x_s.transpose();
        const double norm = lm.x_s.template head<3>().norm();
        lm.x_s.template head<3>() =
            rig_->cameras_[0]->Unproject(lm.z_ref).normalized() * norm;
        // std::cerr << " post x_s: " << lm.x_s.transpose() << std::endl;
        }
      }
    }

  // Update the camera extrinsics if optimized.
  if (kTvsInCalib && delta.delta_k.rows() > 0) {
    StreamMessage(debug_level) << "Prev tvs:\n" << rig_->cameras_[0]->Pose().matrix() <<
                                  std::endl;

    const Eigen::Matrix<Scalar, 6, 1> tvs_delta =
        -delta.delta_k.template segment<6>(kTvsOffset);
    rig_->cameras_[0]->SetPose(exp_decoupled(rig_->cameras_[0]->Pose(), tvs_delta));

    StreamMessage(debug_level) << "Post tvs:\n" << rig_->cameras_[0]->Pose().matrix() <<
                                  std::endl;
  }

    // update poses
    // std::cout << "Updating " << uNumPoses << " active poses." << std::endl;
    for (size_t ii = 0 ; ii < poses_.size() ; ++ii) {
      // only update active poses, as inactive ones are not part of the
      // optimization
      if (poses_[ii].is_active) {
        const uint32_t p_offset = poses_[ii].opt_id*kPoseDim;
        const Eigen::Matrix<Scalar, 6, 1>& p_update =
            -delta.delta_p.template block<6,1>(p_offset,0)*coef;

        poses_[ii].t_wp = exp_decoupled(poses_[ii].t_wp, p_update);

        // update the velocities if they are parametrized
        if (kVelInState) {
          poses_[ii].v_w -=
              delta.delta_p.template block<3,1>(p_offset+6,0)*coef;
        }

        if (kBiasInState) {
          poses_[ii].b -=
              delta.delta_p.template block<6,1>(p_offset+9,0)*coef;
        }

        StreamMessage(debug_level + 1) << "Pose delta for " << ii << " is " <<
                                          (-delta.delta_p.template block<kPoseDim,1>(p_offset,0) *
                                           coef).transpose() << " pose is " << std::endl <<
                                          poses_[ii].t_wp.matrix() << std::endl;
      }

      // clear the vector of Tsw values as they will need to be recalculated
      poses_[ii].t_sw.clear();
    }

    // update the landmarks
    for (size_t ii = 0 ; ii < landmarks_.size() ; ++ii) {
      if (landmarks_[ii].is_active) {
        const Eigen::Matrix<Scalar, kLmDim, 1>& lm_delta =
            delta.delta_l.template segment<kLmDim>(
              landmarks_[ii].opt_id*kLmDim) * coef;

        // std::cerr << "Delta for landmark " << ii << " is " <<
        //   lm_delta.transpose() << std::endl;

        if (kLmDim == 1) {
          landmarks_[ii].x_s.template tail<kLmDim>() -= lm_delta;
          if (landmarks_[ii].x_s[3] < 0) {
            // std::cerr << "Reverting landmark " << ii << " with x_s: " <<
            //             landmarks_[ii].x_s.transpose() << std::endl;
            landmarks_[ii].x_s.template tail<kLmDim>() += lm_delta;
            landmarks_[ii].is_reliable = false;
          }
        } else {
          landmarks_[ii].x_w.template head<kLmDim>() -= lm_delta;
        }
      }
    }
  }

  ////////////////////////////////////////////////////////////////////////////////
  template<typename Scalar,int LmSize, int PoseSize, int CalibSize, bool DoTvs>
  void BundleAdjuster<Scalar, LmSize, PoseSize, CalibSize, DoTvs>::EvaluateResiduals(
      Scalar* proj_error, Scalar* binary_error,
      Scalar* unary_error, Scalar* inertial_error)
  {
    if (proj_error) {
      // Reset the outlier count.
      for (Landmark& lm : landmarks_) {
        lm.num_outlier_residuals = 0;
      }

      *proj_error = 0;
      for (ProjectionResidual& res : proj_residuals_) {
        Landmark& lm = landmarks_[res.landmark_id];
        Pose& pose = poses_[res.x_meas_id];
        Pose& ref_pose = poses_[res.x_ref_id];
        const SE3t t_sw_m = pose.GetTsw(res.cam_id, rig_);
        const SE3t t_ws_r = ref_pose.GetTsw(lm.ref_cam_id,rig_).inverse();

        Eigen::VectorXd backup_params = rig_->cameras_[res.cam_id]->GetParams();
        if (options_.use_per_pose_cam_params) {
          rig_->cameras_[res.cam_id]->SetParams(pose.cam_params);
        }

        const Vector2t p = kLmDim == 3 ?
              rig_->cameras_[res.cam_id]->Transfer3d(
              t_sw_m, lm.x_w.template head<3>(),lm.x_w(3)) :
            rig_->cameras_[res.cam_id]->Transfer3d(
              t_sw_m*t_ws_r, lm.x_s.template head<3>(),lm.x_s(3));

        res.residual = res.z - p;

        if (options_.use_per_pose_cam_params) {
          rig_->cameras_[res.cam_id]->SetParams(backup_params);
        }

        //  std::cout << "res " << res.residual_id << " : pre" << res.residual.norm() <<
        //               " post " << res.residual.norm() * res.weight << std::endl;
        res.mahalanobis_distance = res.residual.squaredNorm() * res.weight;
        *proj_error += res.mahalanobis_distance;
        // If this is an outlier, mark it as such
        if (res.residual.norm() > options_.projection_outlier_threshold) {
          landmarks_[res.landmark_id].num_outlier_residuals++;
        }
      }
    }

    if (unary_error) {
      *unary_error = 0;
      for (UnaryResidual& res : unary_residuals_) {
        const Pose& pose = poses_[res.pose_id];
        // res.residual = SE3t::log(res.t_wp.inverse() * pose.t_wp);
        res.residual = log_decoupled(pose.t_wp, res.t_wp);

        if (!res.use_rotation) {
          res.residual.template tail<3>().setZero();
        }

        res.mahalanobis_distance =
            (res.residual.transpose() * res.cov_inv * res.residual);
        *unary_error += res.mahalanobis_distance;
      }
    }

    if (binary_error) {
      *binary_error = 0;
      for (BinaryResidual& res : binary_residuals_) {
        const Pose& pose1 = poses_[res.x1_id];
        const Pose& pose2 = poses_[res.x2_id];
        res.residual = log_decoupled(pose1.t_wp.inverse() * pose2.t_wp,
                                     res.t_12);

        if (!res.use_rotation) {
          res.residual.template tail<3>().setZero();
        }

        // res.residual = SE3t::log(pose1.t_wp.inverse() * pose2.t_wp * res.t_21);
        res.mahalanobis_distance = res.residual.squaredNorm() * res.weight;
        *binary_error += res.mahalanobis_distance;
      }
    }

    if (inertial_error) {
      *inertial_error = 0;
      for (ImuResidual& res : inertial_residuals_) {
        // set up the initial pose for the integration
        const Vector3t gravity = kGravityInCalib ? GetGravityVector(imu_.g) :
                                                   imu_.g_vec;

        const Pose& pose1 = poses_[res.pose1_id];
        const Pose& pose2 = poses_[res.pose2_id];

        // Eigen::Matrix<Scalar,10,10> jb_y;
        const ImuPose imu_pose = ImuResidual::IntegrateResidual(
              pose1,res.measurements,pose1.b.template head<3>(),
              pose1.b.template tail<3>(),gravity,res.poses);

        const SE3t& t_wb = pose2.t_wp;

        res.residual.setZero();
        // TODO: This is bad, as the error is taken in the world frame. The order
        // of these should be swapped
        res.residual.template head<6>() = log_decoupled(imu_pose.t_wp, t_wb);
        res.residual.template segment<3>(6) = imu_pose.v_w - pose2.v_w;

        if (kBiasInState) {
          res.residual.template segment<6>(9) = pose1.b - pose2.b;
        }

        res.mahalanobis_distance =
            (res.residual.transpose() * res.cov_inv * res.residual);
        *inertial_error += res.mahalanobis_distance;
      }

      if (inertial_residuals_.size() > 0 && translation_enabled_ == false) {
        if (kTvsInCalib) {
          const Scalar log_dif =
              SE3t::log(imu_.t_vs * last_tvs_.inverse()).norm();

          StreamMessage(debug_level) << "logDif is " << log_dif << std::endl;

          if (log_dif < 0.01 && poses_.size() >= 30) {
            StreamMessage(debug_level) << "EMABLING TRANSLATION ERRORS" <<
                                          std::endl;
            translation_enabled_ = true;
          }
          last_tvs_ = imu_.t_vs;
        }
      }
    }

  }

  ////////////////////////////////////////////////////////////////////////////////
  template<typename Scalar,int LmSize, int PoseSize, int CalibSize, bool DoTvs>
  void BundleAdjuster<Scalar, LmSize, PoseSize, CalibSize, DoTvs>::Solve(
      const uint32_t uMaxIter, const Scalar gn_damping,
      const bool error_increase_allowed)
  {
    if (proj_residuals_.empty() && binary_residuals_.empty() &&
        unary_residuals_.empty() && inertial_residuals_.empty()) {
      return;
    }

    // transfor all landmarks to the sensor view
    if (kLmDim == 1) {
      for (Landmark& lm : landmarks_){
        lm.x_s = MultHomogeneous(
              poses_[lm.ref_pose_id].GetTsw(lm.ref_cam_id, rig_) ,lm.x_w);
        // normalize so the ray size is 1
        const Scalar length = lm.x_s.template head<3>().norm();
        lm.x_s = lm.x_s / length;
      }
    }

    for (uint32_t kk = 0 ; kk < uMaxIter ; ++kk) {
      StreamMessage(debug_level) << ">> Iteration " << kk << std::endl;
      StartTimer(_BuildProblem_);
      BuildProblem();
      PrintTimer(_BuildProblem_);


      const uint32_t num_poses = num_active_poses_;
      const uint32_t num_pose_params = num_poses * kPoseDim;
      const uint32_t num_lm = num_active_landmarks_;

      StartTimer(_steup_problem_);
      StartTimer(_rhs_mult_);
      // calculate bp and bl
      rhs_p_.resize(num_pose_params);
      rhs_k_.resize(kCalibDim);
      vi_.resize(num_lm, num_lm);

      VectorXt rhs_p_sc(num_pose_params + kCalibDim);
      jt_l_j_pr_.resize(num_lm, num_poses);

      BlockMat< Eigen::Matrix<Scalar, kPrPoseDim, kLmDim>>
          jt_pr_j_l_vi(num_poses, num_lm);

      s_.resize(num_pose_params + kCalibDim, num_pose_params + kCalibDim);

      PrintTimer(_rhs_mult_);


      StartTimer(_jtj_);
      u_.resize(num_poses, num_poses);

      vi_.setZero();
      u_.setZero();
      rhs_p_.setZero();
      rhs_k_.setZero();
      s_.setZero();
      rhs_p_sc.setZero();

      if (proj_residuals_.size() > 0 && num_poses > 0) {
        BlockMat< Eigen::Matrix<Scalar, kPrPoseDim, kPrPoseDim>> jt_pr_j_pr(
              num_poses, num_poses);
        Eigen::SparseBlockProduct(jt_pr, j_pr_, jt_pr_j_pr,
                                  options_.use_triangular_matrices);

        decltype(u_) temp_u = u_;
        // this is a block add, as jt_pr_j_pr does not have the same block
        // dimensions as u, due to efficiency
        Eigen::template SparseBlockAdd(temp_u, jt_pr_j_pr, u_);

        VectorXt jt_pr_r_pr(num_pose_params);
        // this is a strided multiplication, as jt_pr_r_pr might have a larger
        // pose dimension than jt_pr (for efficiency)
        Eigen::SparseBlockVectorProductDenseResult(jt_pr, r_pr_, jt_pr_r_pr,
                                                   -1, kPoseDim);
        rhs_p_ += jt_pr_r_pr;
      }

      // add the contribution from the binary terms if any
      if (binary_residuals_.size() > 0) {
        BlockMat< Eigen::Matrix<Scalar, kPoseDim, kPoseDim> > jt_pp_j_pp(
              num_poses, num_poses);

        Eigen::SparseBlockProduct(jt_pp_ ,j_pp_, jt_pp_j_pp,
                                  options_.use_triangular_matrices);
        decltype(u_) temp_u = u_;
        Eigen::SparseBlockAdd(temp_u,jt_pp_j_pp,u_);

        VectorXt jt_pp_r_pp(num_pose_params);
        Eigen::SparseBlockVectorProductDenseResult(jt_pp_, r_pp_, jt_pp_r_pp);
        StreamMessage(debug_level) << "Adding binary rhs: "
                                   << jt_pp_r_pp.norm() << std::endl;
        rhs_p_ += jt_pp_r_pp;
      }

      // add the contribution from the unary terms if any
      if (unary_residuals_.size() > 0) {
        BlockMat< Eigen::Matrix<Scalar, kPoseDim, kPoseDim> > jt_u_j_u(
              num_poses, num_poses);

        Eigen::SparseBlockProduct(jt_u_, j_u_, jt_u_j_u,
                                  options_.use_triangular_matrices);
        decltype(u_) temp_u = u_;
        Eigen::SparseBlockAdd(temp_u, jt_u_j_u, u_);

        VectorXt jt_u_r_u(num_pose_params);
        Eigen::SparseBlockVectorProductDenseResult(jt_u_, r_u_, jt_u_r_u);
        rhs_p_ += jt_u_r_u;
      }

      // add the contribution from the imu terms if any
      if (inertial_residuals_.size() > 0) {
        BlockMat< Eigen::Matrix<Scalar, kPoseDim, kPoseDim> > jt_i_j_i(
              num_poses, num_poses);

        Eigen::SparseBlockProduct(jt_i_, j_i_, jt_i_j_i,
                                  options_.use_triangular_matrices);
        decltype(u_) temp_u = u_;
        Eigen::SparseBlockAdd(temp_u, jt_i_j_i, u_);

        VectorXt jt_i_r_i(num_pose_params);
        Eigen::SparseBlockVectorProductDenseResult(jt_i_, r_i_, jt_i_r_i);
        rhs_p_ += jt_i_r_i;
      }

      StreamMessage(debug_level + 1) << "rhs_p_ norm after intertial res: " <<
                                        rhs_p_.squaredNorm() << std::endl;

      PrintTimer(_jtj_);

      StartTimer(_schur_complement_);
      if (kLmDim > 0 && num_lm > 0) {
        rhs_l_.resize(num_lm*kLmDim);
        rhs_l_.setZero();
        StartTimer(_schur_complement_v);
        Eigen::Matrix<Scalar,kLmDim,1> jtr_l;
        for (uint32_t ii = 0; ii < landmarks_.size() ; ++ii) {
          // Skip inactive landmarks.
          if ( !landmarks_[ii].is_active) {
            continue;
          }
          landmarks_[ii].jtj.setZero();
          jtr_l.setZero();
          for (const int id : landmarks_[ii].proj_residuals) {
            const ProjectionResidual& res = proj_residuals_[id];
            landmarks_[ii].jtj += (res.dz_dlm.transpose() * res.dz_dlm) *
                res.weight;
            jtr_l += (res.dz_dlm.transpose() * sqrt(res.weight) *
                      r_pr_.template block<ProjectionResidual::kResSize,1>(
                        res.residual_id*ProjectionResidual::kResSize, 0));
          }
          rhs_l_.template block<kLmDim,1>(landmarks_[ii].opt_id*kLmDim, 0) =
              jtr_l;
          if (kLmDim == 1) {
            if (fabs(landmarks_[ii].jtj(0,0)) < 1e-6) {
              landmarks_[ii].jtj(0,0) += 1e-6;
            }
          } else {
            if (landmarks_[ii].jtj.norm() < 1e-6) {
              landmarks_[ii].jtj.diagonal() +=
                  Eigen::Matrix<Scalar, kLmDim, 1>::Constant(1e-6);
            }
          }
          vi_.insert(landmarks_[ii].opt_id, landmarks_[ii].opt_id) =
              landmarks_[ii].jtj.inverse();
        }

        PrintTimer(_schur_complement_v);

        // we only do this if there are active poses
        if (num_poses > 0) {
          StartTimer(_schur_complement_jtpr_jl);
          jt_pr_j_l_.resize(num_poses, num_lm);

          Eigen::SparseBlockProduct(jt_pr,j_l_,jt_pr_j_l_);

          decltype(jt_l_j_pr_)::forceTranspose(jt_pr_j_l_, jt_l_j_pr_);
          PrintTimer(_schur_complement_jtpr_jl);

          // attempt to solve for the poses. W_V_inv is used later on,
          // so we cache it
          StartTimer(_schur_complement_jtpr_jl_vi);
          Eigen::SparseBlockDiagonalRhsProduct(jt_pr_j_l_, vi_, jt_pr_j_l_vi);
          PrintTimer(_schur_complement_jtpr_jl_vi);


          StartTimer(_schur_complement_jtpr_jl_vi_jtl_jpr);
          BlockMat< Eigen::Matrix<Scalar, kPrPoseDim, kPrPoseDim>>
              jt_pr_j_l_vi_jt_l_j_pr(num_poses, num_poses);

          Eigen::SparseBlockProduct(jt_pr_j_l_vi, jt_l_j_pr_,
                                    jt_pr_j_l_vi_jt_l_j_pr,
                                    options_.use_triangular_matrices);
          PrintTimer(_schur_complement_jtpr_jl_vi_jtl_jpr);

          Eigen::SparseBlockSubtractDenseResult(
                u_, jt_pr_j_l_vi_jt_l_j_pr,
                s_.template block(
                  0, 0, num_pose_params,
                  num_pose_params ));

          // now form the rhs for the pose equations
          VectorXt jt_pr_j_l_vi_bll(num_pose_params);
          Eigen::SparseBlockVectorProductDenseResult(
                jt_pr_j_l_vi, rhs_l_, jt_pr_j_l_vi_bll, -1, kPoseDim);

          rhs_p_sc.template head(num_pose_params) = rhs_p_ - jt_pr_j_l_vi_bll;
        }
      } else {
        Eigen::LoadDenseFromSparse(
              u_, s_.template block(0, 0, num_pose_params, num_pose_params));
        rhs_p_sc.template head(num_pose_params) = rhs_p_;
      }
      PrintTimer(_schur_complement_);

      if(kJkprUsed){
        BlockMat< Eigen::Matrix<Scalar, kCalibDim, kCalibDim>> jt_kpr_j_kpr(1, 1);
        Eigen::SparseBlockProduct(jt_kpr_, j_kpr_, jt_kpr_j_kpr);
        MatrixXt djt_kpr_j_kpr(kCalibDim, kCalibDim);
        Eigen::LoadDenseFromSparse(jt_kpr_j_kpr, djt_kpr_j_kpr);
        s_.template block<kCalibDim, kCalibDim>(num_pose_params, num_pose_params)
            += djt_kpr_j_kpr;

        BlockMat<Eigen::Matrix<Scalar, kPrPoseDim, kCalibDim>>
            jt_pr_j_kpr(num_poses, 1);

        Eigen::SparseBlockProduct(jt_pr, j_kpr_, jt_pr_j_kpr);

        MatrixXt djt_pr_j_kpr(kPoseDim * num_poses, kCalibDim);
        // This is a strided load operation, to match kPrPoseDim to kPoseDim
        Eigen::LoadDenseFromSparse<decltype(jt_pr_j_kpr),
            MatrixXt,
            kPoseDim, kCalibDim>
            (jt_pr_j_kpr, djt_pr_j_kpr);
        // std::cerr << "djt_pr_j_kpr: " << djt_pr_j_kpr << std::endl;
        s_.template block(0, num_pose_params, num_pose_params, kCalibDim) +=
            djt_pr_j_kpr;
        if (!options_.use_triangular_matrices) {
          s_.template block(num_pose_params, 0, kCalibDim, num_pose_params) +=
              djt_pr_j_kpr.transpose();
        }

        VectorXt jt_kpr_r_pr(kCalibDim, 1);
        Eigen::SparseBlockVectorProductDenseResult(jt_kpr_, r_pr_, jt_kpr_r_pr);
        rhs_k_ += jt_kpr_r_pr;
      }

      // Assign the calibration parameter RHS vector.
      if (kCalibDim) {
        rhs_p_sc.template tail<kCalibDim>() = rhs_k_;
      }

      // Do the schur complement with the calibration parameters.
      if(kJkprUsed && kLmDim > 0 && num_lm > 0) {
        jt_l_j_kpr_.resize(num_lm, 1);
        // schur complement
        BlockMat< Eigen::Matrix<Scalar, kCalibDim, kLmDim>> jt_kpr_jl(1, num_lm);
        Eigen::SparseBlockProduct(jt_kpr_, j_l_, jt_kpr_jl);
        decltype(jt_l_j_kpr_)::forceTranspose(jt_kpr_jl, jt_l_j_kpr_);

        MatrixXt djt_pr_j_l_vi_jt_l_j_kpr(kPoseDim * num_poses, kCalibDim);
        BlockMat<Eigen::Matrix<Scalar, kPrPoseDim, kCalibDim>>
            jt_pr_j_l_vi_jt_l_j_kpr(num_poses, 1);
        jt_pr_j_l_vi_jt_l_j_kpr.setZero();

        Eigen::SparseBlockProduct(
              jt_pr_j_l_vi, jt_l_j_kpr_, jt_pr_j_l_vi_jt_l_j_kpr);
        // This is a strided load operation, to match kPrPoseDim to kPoseDim
        Eigen::LoadDenseFromSparse<decltype(jt_pr_j_l_vi_jt_l_j_kpr),
            MatrixXt,
            kPoseDim, kCalibDim>(
              jt_pr_j_l_vi_jt_l_j_kpr, djt_pr_j_l_vi_jt_l_j_kpr);

        s_.template block(0, num_pose_params, num_pose_params, kCalibDim) -=
            djt_pr_j_l_vi_jt_l_j_kpr;
        if (!options_.use_triangular_matrices) {
          s_.template block(num_pose_params, 0, kCalibDim, num_pose_params) -=
              djt_pr_j_l_vi_jt_l_j_kpr.transpose();
        }

        BlockMat<Eigen::Matrix<Scalar, kCalibDim, kLmDim>>
            jt_kpr_j_l_vi(1, num_lm);
        Eigen::SparseBlockProduct(jt_kpr_jl, vi_, jt_kpr_j_l_vi);

        BlockMat<Eigen::Matrix<Scalar, kCalibDim, kCalibDim>>
            jt_kpr_j_l_vi_jt_l_j_kpr(1, 1);
        Eigen::SparseBlockProduct(
              jt_kpr_j_l_vi,
              jt_l_j_kpr_,
              jt_kpr_j_l_vi_jt_l_j_kpr);

        MatrixXt djt_kpr_j_l_vi_jt_l_j_kpr(kCalibDim, kCalibDim);
        Eigen::LoadDenseFromSparse(
              jt_kpr_j_l_vi_jt_l_j_kpr,
              djt_kpr_j_l_vi_jt_l_j_kpr);

        s_.template block<kCalibDim, kCalibDim>(num_pose_params, num_pose_params)
            -= djt_kpr_j_l_vi_jt_l_j_kpr;

        VectorXt jt_kpr_j_l_vi_bl;
        jt_kpr_j_l_vi_bl.resize(kCalibDim);
        Eigen::SparseBlockVectorProductDenseResult(
              jt_kpr_j_l_vi, rhs_l_, jt_kpr_j_l_vi_bl);

        rhs_p_sc.template tail<kCalibDim>() -= jt_kpr_j_l_vi_bl;
      }


      // regularize masked parameters.
      if (is_param_mask_used_) {
        for (Pose& pose : poses_) {
          if (pose.is_active && pose.is_param_mask_used) {
            for (uint32_t ii = 0 ; ii < pose.param_mask.size() ; ++ii) {
              if (!pose.param_mask[ii]) {
                const int idx = pose.opt_id*kPoseDim + ii;
                s_(idx, idx) = 1e6;
              }
            }
          }
        }
      }

      if (options_.write_reduced_camera_matrix) {
        std::cerr << "Writing reduced camera matrix for " << num_pose_params <<
                     " pose parameters and " << kCalibDim << " calib "
                                                             " parameters " << std::endl;
        std::ofstream("s.txt", std::ios_base::trunc) << s_.format(kLongCsvFmt);
        std::ofstream("rhs.txt", std::ios_base::trunc) << rhs_p_sc.format(kLongCsvFmt);

        MatrixXt dj_pr(j_pr_.rows() * ProjectionResidual::kResSize,
                       j_pr_.cols() * kPrPoseDim);
        Eigen::LoadDenseFromSparse(j_pr_, dj_pr);
        std::ofstream("j_pr.txt", std::ios_base::trunc) << dj_pr.format(kLongCsvFmt);

        std::ofstream("r_pr.txt", std::ios_base::trunc) << r_pr_.format(kLongCsvFmt);

        MatrixXt dj_l(j_l_.rows() * ProjectionResidual::kResSize,
                      j_l_.cols() * kLmDim);
        Eigen::LoadDenseFromSparse(j_l_, dj_l);
        std::ofstream("j_l.txt", std::ios_base::trunc) << dj_l.format(kLongCsvFmt);

        MatrixXt dj_kpr(j_kpr_.rows() * ProjectionResidual::kResSize,
                        j_kpr_.cols() * kCalibDim);
        Eigen::LoadDenseFromSparse(j_kpr_, dj_kpr);
        std::ofstream("j_kpr.txt", std::ios_base::trunc) << dj_kpr.format(kLongCsvFmt);

        MatrixXt djt_kpr = dj_kpr.transpose();
        MatrixXt djt_kpr_dj_kpr = (djt_kpr * dj_kpr).eval();
        std::ofstream("jt_kpr_j_kpr.txt", std::ios_base::trunc) << djt_kpr_dj_kpr.format(kLongCsvFmt);
      }

      PrintTimer(_steup_problem_);

      // now we have to solve for the pose constraints
      StartTimer(_solve_);
      // Precompute the sparse s matrix if necessary.
      if (options_.use_sparse_solver) {
        s_sparse_ = s_.sparseView();
      }

      // std::cout << "running solve internal with " << use_dogleg << std::endl;
      if (!SolveInternal(rhs_p_sc, gn_damping, error_increase_allowed,
                         options_.use_dogleg)) {
        StreamMessage(debug_level) << "Exiting due to error increase." <<
                                      std::endl;
        break;
      }

      PrintTimer(_solve_);

      if ((fabs(summary_.post_solve_norm - summary_.pre_solve_norm) /
           summary_.pre_solve_norm) < options_.error_change_threshold ) {
        StreamMessage(debug_level) << "Exiting due to error change too small." <<
                                      std::endl;
        summary_.result = ErrorChangeBelowThreshold;
        break;
      }

      if (summary_.delta_norm < options_.param_change_threshold) {
        StreamMessage(debug_level) << "Exiting due to param change too small." <<
                                      std::endl;
        summary_.result = ParamChangeBelowThreshold;
        break;
      }

    }


    if (kBiasInState && poses_.size() > 0) {
      imu_.b_g = poses_.back().b.template head<3>();
      imu_.b_a = poses_.back().b.template tail<3>();
    }

    // after the solve transfor all landmarks to world view
    if (kLmDim == 1) {
      for (Landmark& lm : landmarks_){
        lm.x_w = MultHomogeneous(
              poses_[lm.ref_pose_id].GetTsw(lm.ref_cam_id, rig_).inverse(),
            lm.x_s);
      }
    }

    // Now go through any conditioned residuals, and figure out the total
    // mahalonobis distance for the conditioning
    summary_.cond_inertial_error = 0;
    summary_.cond_proj_error = 0;
    summary_.num_cond_inertial_residuals =
        conditioning_inertial_residuals_.size();
    summary_.num_inertial_residuals = inertial_residuals_.size();
    summary_.inertial_error = inertial_error_;
    for (uint32_t id : conditioning_inertial_residuals_) {
      const ImuResidual& res = inertial_residuals_[id];
      summary_.cond_inertial_error += res.mahalanobis_distance;
    }

    /*for (const ImuResidual& res : inertial_residuals_) {
    std::cerr << "Mahalanobis dist. for residual " << res.residual_id <<
                 " : " << res.mahalanobis_distance << std::endl;
  }*/

    summary_.num_cond_proj_residuals = conditioning_proj_residuals_.size();
    summary_.num_proj_residuals = proj_residuals_.size();
    summary_.proj_error_ = proj_error_;
    for (uint32_t id : conditioning_proj_residuals_) {
      const ProjectionResidual& res = proj_residuals_[id];
      summary_.cond_proj_error += res.mahalanobis_distance / res.weight;
    }
  }

  ////////////////////////////////////////////////////////////////////////////////
  template<typename Scalar,int LmSize, int PoseSize, int CalibSize, bool DoTvs>
  void BundleAdjuster<Scalar, LmSize, PoseSize, CalibSize, DoTvs>::GetLandmarkDelta(
      const Delta& delta,  const uint32_t num_poses, const uint32_t num_lm,
      VectorXt& delta_l)
  {
    StartTimer(_back_substitution_);
    if (num_lm > 0) {
      delta_l.resize(num_lm*kLmDim);
      VectorXt rhs_l_sc =  rhs_l_;

      if (num_poses > 0) {
        VectorXt wt_delta_p_k;
        wt_delta_p_k.resize(num_lm * kLmDim);
        // this is the strided multiplication as delta_p has all pose parameters,
        // however jt_l_j_pr_delta_p is only with respect to the 6 pose parameters
        Eigen::SparseBlockVectorProductDenseResult(
              jt_l_j_pr_, delta.delta_p, wt_delta_p_k, kPoseDim, -1);

        rhs_l_sc.resize(num_lm*kLmDim);
        rhs_l_sc -=  wt_delta_p_k;

        if (kJkprUsed) {
          Eigen::SparseBlockVectorProductDenseResult(
                jt_l_j_kpr_, delta.delta_k, wt_delta_p_k);
          rhs_l_sc -=  wt_delta_p_k;
        }
      }

      for (size_t ii = 0 ; ii < num_lm ; ++ii) {
        delta_l.template block<kLmDim,1>( ii*kLmDim, 0 ).noalias() =
            vi_.coeff(ii, ii) *
            rhs_l_sc.template block<kLmDim, 1>(ii * kLmDim, 0);
      }
    }
    PrintTimer(_back_substitution_);

  }

  ////////////////////////////////////////////////////////////////////////////////
  template<typename Scalar,int LmSize, int PoseSize, int CalibSize, bool DoTvs>
  void BundleAdjuster<Scalar, LmSize, PoseSize, CalibSize, DoTvs>::CalculateGn(
      const VectorXt& rhs_p, Delta& delta)
  {
    summary_.result = Success;
    if (options_.use_sparse_solver) {
      Eigen::SimplicialLDLT<Eigen::SparseMatrix<Scalar>, Eigen::Upper>
          solver;
      solver.compute(s_sparse_);
      if (solver.info() != Eigen::Success) {
        std::cerr << "SimplicialLDLT FAILED!" << std::endl;
        summary_.result = FactorizationError;
      }
      if (rhs_p.rows() != 0) {
        VectorXt delta_p_k = solver.solve(rhs_p);
        if (solver.info() != Eigen::Success) {
          std::cerr << "SimplicialLDLT SOLVE FAILED!" << std::endl;
          summary_.result = SolverError;
        }
        const uint32_t num_pose_params = delta_p_k.rows() - kCalibDim;
        delta.delta_p = delta_p_k.head(num_pose_params);
        if (kCalibDim) {
          delta.delta_k = delta_p_k.tail(kCalibDim);

          if (options_.calculate_calibration_marginals) {
            MatrixXt cov(delta_p_k.rows(), kCalibDim);
            for (uint32_t ii = 0; ii < kCalibDim ; ++ii) {
              VectorXt res = solver.solve(
                    VectorXt::Unit(rhs_p.rows(), num_pose_params + ii));
              if (solver.info() != Eigen::Success) {
                std::cerr << " Cov calculation: SimplicialLDLT SOLVE FAILED!" <<
                             std::endl;
              }
              cov.col(ii) = res;
            }
            summary_.calibration_marginals =
                cov.template bottomRightCorner<kCalibDim, kCalibDim>();
          }
        }
      } else {
        delta.delta_p = VectorXt();
        delta.delta_k = VectorXt();
      }

    } else {
      Eigen::LDLT<Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic>,
          Eigen::Upper> solver;
      solver.compute(s_);
      if (solver.info() != Eigen::Success) {
        std::cerr << "LDLT FAILED!" << std::endl;
      }
      if (rhs_p.rows() != 0) {
        const VectorXt delta_p_k = solver.solve(rhs_p);
        if (solver.info() != Eigen::Success) {
          std::cerr << "LDLT SOLVE FAILED!" << std::endl;
        }
        const uint32_t num_pose_params = delta_p_k.rows() - kCalibDim;
        delta.delta_p = delta_p_k.head(num_pose_params);
        if (kCalibDim) {
          delta.delta_k = delta_p_k.tail(kCalibDim);

          if (options_.calculate_calibration_marginals) {
            MatrixXt cov(delta_p_k.rows(), kCalibDim);
            for (uint32_t ii = 0; ii < kCalibDim ; ++ii) {
              VectorXt res = solver.solve(
                    VectorXt::Unit(rhs_p.rows(), num_pose_params + ii));
              if (solver.info() != Eigen::Success) {
                std::cerr << "LDLT SOLVE FAILED!" << std::endl;
              }
              cov.col(ii) = res;
            }
            summary_.calibration_marginals =
                cov.template bottomRightCorner<kCalibDim, kCalibDim>();
          }
        }
      } else {
        delta.delta_p = VectorXt();
        delta.delta_k = VectorXt();
      }
    }

    // Do rank revealing QR
    // Eigen::FullPivHouseholderQR<Eigen::Matrix<Scalar,
    //                                          Eigen::Dynamic, Eigen::Dynamic>> qr;
    // qr.compute(s_);
    // std::cout << "S dim: " << s_.rows() << " rank: " << qr.rank() << std::endl;
  }


  ////////////////////////////////////////////////////////////////////////////////
  template<typename Scalar,int LmSize, int PoseSize, int CalibSize, bool DoTvs>
  bool BundleAdjuster<Scalar, LmSize, PoseSize, CalibSize, DoTvs>::SolveInternal(
      VectorXt rhs_p_sc, const Scalar gn_damping,
      const bool error_increase_allowed, const bool use_dogleg
      )
  {
    // std::cout << "ub solve internal with " << use_dogleg << std::endl;
    bool gn_computed = false;
    Delta delta_sd;
    Delta delta_dl;
    Delta delta_gn;
    Scalar proj_error, binary_error, unary_error, inertial_error;

    if (use_dogleg) {
      // Refer to:
      // http://people.csail.mit.edu/kaess/pub/Rosen12icra.pdf
      // is levenberg-marquardt the most efficient optimization algorithm
      // for implementing bundle adjustment
      // TODO: make sure the sd solution here is consistent with the covariances

      // calculate steepest descent result
      Scalar numerator = rhs_p_.squaredNorm() + rhs_l_.squaredNorm() +
          rhs_k_.squaredNorm();

      VectorXt j_p_rhs_p(ProjectionResidual::kResSize * proj_residuals_.size());
      j_p_rhs_p.setZero();
      VectorXt j_kp_rhs_k(ProjectionResidual::kResSize * proj_residuals_.size());
      j_kp_rhs_k.setZero();
      VectorXt j_pp_rhs_p(BinaryResidual::kResSize * binary_residuals_.size());
      j_pp_rhs_p.setZero();
      VectorXt j_u_rhs_p(UnaryResidual::kResSize * unary_residuals_.size());
      j_u_rhs_p.setZero();
      VectorXt j_i_rhs_p(ImuResidual::kResSize * inertial_residuals_.size());
      j_i_rhs_p.setZero();
      VectorXt j_l_rhs_l(ProjectionResidual::kResSize * proj_residuals_.size());
      j_l_rhs_l.setZero();

      StreamMessage(debug_level + 1) << "rhs_p_ norm: " << rhs_p_.squaredNorm() <<
                                        std::endl;
      StreamMessage(debug_level + 1) << "rhs_l_ norm: " << rhs_l_.squaredNorm() <<
                                        std::endl;

      if (num_active_poses_ > 0) {
        if (proj_residuals_.size() > 0) {
          Eigen::SparseBlockVectorProductDenseResult(j_pr_, rhs_p_, j_p_rhs_p,
                                                     kPoseDim);
          if (kJkprUsed) {
            Eigen::SparseBlockVectorProductDenseResult(j_kpr_, rhs_k_,
                                                       j_kp_rhs_k);
          }
        }

        if (inertial_residuals_.size() > 0) {
          Eigen::SparseBlockVectorProductDenseResult(j_i_, rhs_p_, j_i_rhs_p);
        }

        if (binary_residuals_.size() > 0) {
          Eigen::SparseBlockVectorProductDenseResult(j_pp_, rhs_p_, j_pp_rhs_p);
        }

        if (unary_residuals_.size() > 0) {
          Eigen::SparseBlockVectorProductDenseResult(j_u_, rhs_p_, j_u_rhs_p);
        }
      }

      if (num_active_landmarks_ > 0 && proj_residuals_.size() > 0) {
        Eigen::SparseBlockVectorProductDenseResult(j_l_, rhs_l_, j_l_rhs_l);
      }

      Scalar denominator = (j_p_rhs_p + j_l_rhs_l).squaredNorm() +
          j_pp_rhs_p.squaredNorm() +
          j_u_rhs_p.squaredNorm() +
          j_i_rhs_p.squaredNorm() +
          j_kp_rhs_k.squaredNorm();

      StreamMessage(debug_level + 1) << "j_p_rhs_p norm: " <<
                                        j_p_rhs_p.squaredNorm() << std::endl;
      StreamMessage(debug_level + 1) << "j_l_rhs_l norm: " <<
                                        j_l_rhs_l.squaredNorm() << std::endl;
      StreamMessage(debug_level + 1) << "j_i_rhs_p norm: " <<
                                        j_i_rhs_p.squaredNorm() << std::endl;

      Scalar factor = numerator/denominator;
      StreamMessage(debug_level + 1) << "factor: " << factor <<
                                        " nom: " << numerator << " denom: " <<
                                        denominator << std::endl;
      delta_sd.delta_p = rhs_p_ * factor;
      delta_sd.delta_k = rhs_k_ * factor;
      delta_sd.delta_l = rhs_l_ * factor;

      // now calculate the steepest descent norm
      Scalar delta_sd_norm = sqrt(delta_sd.delta_p.squaredNorm() +
                                  delta_sd.delta_l.squaredNorm());
      StreamMessage(debug_level + 1) << "sd norm : " << delta_sd_norm <<
                                        std::endl;

      uint32_t iteration_count = 0;
      while (1) {
        iteration_count++;
        if (iteration_count > options_.dogleg_max_inner_iterations) {
          StreamMessage(debug_level) <<
                                        "Maximum number of inner iterations reached." << std::endl;
          break;
        }
        if (delta_sd_norm > trust_region_size_ &&
            trust_region_size_ != kTrustRegionAuto) {
          StreamMessage(debug_level) <<
                                        "sd norm larger than trust region of " <<
                                        trust_region_size_ << " chosing sd update " << std::endl;

          Scalar factor = trust_region_size_ / delta_sd_norm;
          delta_dl.delta_p = factor * delta_sd.delta_p;
          delta_dl.delta_k = factor * delta_sd.delta_k;
          delta_dl.delta_l = factor * delta_sd.delta_l;
        }else {
          StreamMessage(debug_level) <<
                                        "sd norm less than trust region of " <<
                                        trust_region_size_ << std::endl;

          if (!gn_computed) {
            StreamMessage(debug_level + 1) << "Computing gauss newton " <<
                                              std::endl;
            if (num_active_poses_ > 0) {
              CalculateGn(rhs_p_sc, delta_gn);
              if (!summary_.IsResultGood()) {
                return false;
              }
            }
            // now back substitute the landmarks
            GetLandmarkDelta(delta_gn,  num_active_poses_,
                             num_active_landmarks_, delta_gn.delta_l);
            gn_computed = true;
          }

          Scalar delta_gn_norm = sqrt(delta_gn.delta_p.squaredNorm() +
                                      delta_gn.delta_k.squaredNorm() +
                                      delta_gn.delta_l.squaredNorm());
          const bool delta_gn_good =
              !std::isnan(delta_gn_norm) && !std::isinf(delta_gn_norm);
          if (delta_gn_good && trust_region_size_ == kTrustRegionAuto) {
            trust_region_size_ = delta_gn_norm;
          }

          if (delta_gn_good && delta_gn_norm <= trust_region_size_) {
            StreamMessage(debug_level) <<
                                          "Gauss newton delta: " << delta_gn_norm << "is smaller than trust "
                                                                                     "region of " << trust_region_size_ << std::endl;

            delta_dl = delta_gn;
          } else {
            StreamMessage(debug_level) <<
                                          "Gauss newton delta: " << delta_gn_norm << " is larger than trust "
                                                                                     "region of " << trust_region_size_ << std::endl;

            VectorXt diff_p = delta_gn.delta_p - delta_sd.delta_p;
            VectorXt diff_k = delta_gn.delta_k - delta_sd.delta_k;
            VectorXt diff_l = delta_gn.delta_l - delta_sd.delta_l;
            Scalar a = diff_p.squaredNorm() + diff_l.squaredNorm() +
                diff_k.squaredNorm();
            Scalar b = 2 * (diff_p.transpose() * delta_sd.delta_p +
                            diff_k.transpose() * delta_sd.delta_k +
                            diff_l.transpose() * delta_sd.delta_l)[0];

            // std::cout << "tr: " << trust_region_size_ << std::endl;
            Scalar c = (delta_sd.delta_p.squaredNorm() +
                        delta_sd.delta_k.squaredNorm() +
                        delta_sd.delta_l.squaredNorm()) -
                trust_region_size_ * trust_region_size_;

            Scalar beta = 0;
            if (b * b > 4 * a * c && a > 1e-10) {
              beta = (-(b*b) + sqrt(b*b - 4*a*c)) / (2 * a);
            } else {
              StreamMessage(debug_level) <<
                                            "Cannot calculate blending factor. Using sd - a:" << a << " b:" <<
                                            b << " c:" << c << std::endl;
            }

            delta_dl.delta_p = delta_sd.delta_p + beta*(diff_p);
            delta_dl.delta_k = delta_sd.delta_k+ beta*(diff_k);
            delta_dl.delta_l = delta_sd.delta_l + beta*(diff_l);
          }
        }

        // Make copies of the initial parameters.
        decltype(landmarks_) landmarks_copy = landmarks_;
        decltype(poses_) poses_copy = poses_;
        decltype(imu_) imu_copy = imu_;
        Eigen::VectorXd params_backup;
        if (rig_->cameras_.size() != 0) {
          params_backup = rig_->cameras_[0]->GetParams();
        }
        // decltype(rig_) rig_copy = rig_;


        // We have to calculate the residuals here, as during the inner loop of
        // dogleg, the residuals are constantly changing.
        EvaluateResiduals(&proj_error, &binary_error,
                          &unary_error, &inertial_error);
        summary_.pre_solve_norm = proj_error + inertial_error + binary_error +
            unary_error;
        if (options_.apply_results) {
          ApplyUpdate(delta_dl, false);

          StreamMessage(debug_level) << std::setprecision (15) <<
                                        "Pre-solve norm: " << summary_.pre_solve_norm << " with Epr:" <<
                                        proj_error << " and Ei:" << inertial_error <<
                                        " and Epp: " << binary_error << " and Eu " << unary_error <<
                                        std::endl;
        }

        EvaluateResiduals(&proj_error, &binary_error,
                          &unary_error, &inertial_error);
        summary_.post_solve_norm = proj_error + inertial_error + binary_error +
            unary_error;


        StreamMessage(debug_level) << std::setprecision (15) <<
                                      "Post-solve norm: " << summary_.post_solve_norm << " update delta: " <<
                                      summary_.delta_norm << " with Epr:" << proj_error << " and Ei:" <<
                                      inertial_error << " and Epp: " << binary_error << " and Eu " <<
                                      unary_error << std::endl;

        if (summary_.post_solve_norm > summary_.pre_solve_norm) {
          if (options_.apply_results) {
            landmarks_ = landmarks_copy;
            poses_ = poses_copy;
            imu_ = imu_copy;
            if (rig_->cameras_.size() != 0) {
              rig_->cameras_[0]->SetParams(params_backup);
            }
          }

          trust_region_size_ /= 2;
          StreamMessage(debug_level) << "Error increased, reducing "
                                        "trust region to " << trust_region_size_ << std::endl;
        } else {
          proj_error_ = proj_error;
          unary_error_ = unary_error;
          binary_error_ = binary_error;
          inertial_error_ = inertial_error;
          trust_region_size_ *= 2;
          StreamMessage(debug_level) << "Error decreased, increasing "
                                        "trust region to " << trust_region_size_ << std::endl;
          break;
        }
      }
    } else {
      // If not doing dogleg, just do straight-up Gauss-Newton.
      StreamMessage(debug_level) << "NOT USING DOGLEG" << std::endl;

      Delta delta;
      if (num_active_poses_ > 0) {
        CalculateGn(rhs_p_sc, delta);
        if (!summary_.IsResultGood()) {
          return false;
        }
      }

      decltype(landmarks_) landmarks_copy = landmarks_;
      decltype(poses_) poses_copy = poses_;
      decltype(imu_) imu_copy = imu_;
      Eigen::VectorXd params_backup;
      if (rig_->NumCams() != 0) {
        params_backup = rig_->cameras_[0]->GetParams();
      }

      // now back substitute the landmarks
      GetLandmarkDelta(delta, num_active_poses_, num_active_landmarks_,
                       delta.delta_l);

      delta.delta_l *= gn_damping;
      delta.delta_k *= gn_damping;
      delta.delta_p *= gn_damping;


      // We have to calculate the residuals here, as during the inner loop of
      // dogleg, the residuals are constantly changing.
      EvaluateResiduals(&proj_error, &binary_error,
                        &unary_error, &inertial_error);
      const Scalar prev_error = proj_error + inertial_error + binary_error +
          unary_error;
      if (options_.apply_results) {
        ApplyUpdate(delta, false);

        StreamMessage(debug_level) << std::setprecision (15) <<
                                      "Pre-solve norm: " << prev_error << " with Epr:" <<
                                      proj_error << " and Ei:" << inertial_error <<
                                      " and Epp: " << binary_error << " and Eu " << unary_error << std::endl;
      }

      Scalar proj_error, binary_error, unary_error, inertial_error;
      EvaluateResiduals(&proj_error, &binary_error,
                        &unary_error, &inertial_error);
      const Scalar postError = proj_error + inertial_error + binary_error +
          unary_error;

      StreamMessage(debug_level) << std::setprecision (15) <<
                                    "Post-solve norm: " << postError << " with Epr:" <<
                                    proj_error << " and Ei:" << inertial_error <<
                                    " and Epp: " << binary_error << " and Eu " << unary_error << std::endl;

      if (postError > prev_error && !error_increase_allowed) {
        StreamMessage(debug_level) << "Error increasing during optimization, "
                                      " rolling back .." << std::endl;
        if (options_.apply_results) {
          landmarks_ = landmarks_copy;
          poses_ = poses_copy;
          imu_ = imu_copy;
          if (rig_->NumCams() != 0) {
            rig_->cameras_[0]->SetParams(params_backup);
          }
        }
        summary_.result = ErrorIncreased;

        return false;
      } else {
        proj_error_ = proj_error;
        unary_error_ = unary_error;
        binary_error_ = binary_error;
        inertial_error_ = inertial_error;
      }
    }
    return true;
  }


  ////////////////////////////////////////////////////////////////////////////////
  template<typename Scalar,int LmSize, int PoseSize, int CalibSize, bool DoTvs>
  void BundleAdjuster<Scalar, LmSize, PoseSize, CalibSize, DoTvs>::BuildProblem()
  {
    // resize as needed
    const uint32_t num_poses = num_active_poses_;
    const uint32_t num_lm = num_active_landmarks_;
    const uint32_t num_proj_res = proj_residuals_.size();
    const uint32_t num_bin_res = binary_residuals_.size();
    const uint32_t num_un_res = unary_residuals_.size();
    const uint32_t num_im_res= inertial_residuals_.size();

    if (num_proj_res > 0) {
      j_pr_.resize(num_proj_res, num_poses);
      jt_pr.resize(num_poses, num_proj_res);
      j_l_.resize(num_proj_res, num_lm);
      // jt_l_.resize(num_lm, num_proj_res);
      r_pr_.resize(num_proj_res*ProjectionResidual::kResSize);

      // these calls remove all the blocks, but KEEP allocated memory as long as
      // the object is alive
      j_pr_.setZero();
      jt_pr.setZero();
      r_pr_.setZero();
      j_l_.setZero();

      if (kJkprUsed) {
        j_kpr_.resize(num_proj_res, 1);
        jt_kpr_.resize(1, num_proj_res);
        j_kpr_.setZero();
        jt_kpr_.setZero();
      }
    }

    if (num_bin_res > 0) {
      j_pp_.resize(num_bin_res, num_poses);
      jt_pp_.resize(num_poses, num_bin_res);
      r_pp_.resize(num_bin_res*BinaryResidual::kResSize);

      j_pp_.setZero();
      jt_pp_.setZero();
      r_pp_.setZero();
    }

    if (num_un_res > 0) {
      j_u_.resize(num_un_res, num_poses);
      jt_u_.resize(num_poses, num_un_res);
      r_u_.resize(num_un_res*UnaryResidual::kResSize);

      j_u_.setZero();
      jt_u_.setZero();
      r_u_.setZero();
    }

    if (num_im_res > 0) {
      j_i_.resize(num_im_res, num_poses);
      jt_i_.resize(num_poses, num_im_res);
      r_i_.resize(num_im_res*ImuResidual::kResSize);

      j_i_.setZero();
      jt_i_.setZero();
      r_i_.setZero();

      if (kTvsInCalib) {
        j_ki_.resize(num_im_res, 1);
        jt_ki_.resize(1, num_im_res);
        j_ki_.setZero();
        jt_ki_.setZero();
      }
    }

    // jt_l_.setZero();

    is_param_mask_used_ = false;

    // go through all the poses to check if they are all active
    bool are_all_active = true;
    for (Pose& pose : poses_) {
      for (int ii = 0; ii < rig_->cameras_.size(); ++ii) {
        pose.GetTsw(ii, rig_);
      }
      if (pose.is_active == false) {
        are_all_active = false;
        break;
      }

      // If for some reason a pose has no constraints, regularize it so the
      // hessian does not become singular.
      if (pose.proj_residuals.size() == 0 &&
          pose.binary_residuals.size() == 0 &&
          pose.unary_residuals.size() == 0 &&
          pose.inertial_residuals.size() == 0) {
        pose.is_param_mask_used = true;
        pose.param_mask.assign(kPoseDim, false);
      }
    }

    // If we are doing an inertial run, and any poses have no inertial constraints
    // we must regularize their velocity and (if applicable) biases.
    if (kVelInState) {
      for (Pose& pose : poses_) {
        if (pose.inertial_residuals.empty() && pose.is_active) {
          StreamMessage(debug_level) <<
                                        "Pose id " << pose.id << " found with no inertial residuals. "
                                                                 " regularizing velocities and biases. " << std::endl;
          pose.is_param_mask_used = true;
          pose.param_mask.assign(kPoseDim, true);
          pose.param_mask[6] = pose.param_mask[7]  = pose.param_mask[8] = false;
          if (kBiasInState) {
            pose.param_mask[9] = pose.param_mask[10] =
                pose.param_mask[11] = pose.param_mask[12] =
                pose.param_mask[13] = pose.param_mask[14] = false;
          }
        }
      }
    }

    // If all poses are active and we are not marginalizing (therefore, we have
    // no prior) then regularize some parameters by setting the parameter mask.
    // This in effect removes these parameters from the optimization, by setting
    // any jacobians to zero and regularizing the hessian diagonal.
    if (are_all_active && num_un_res == 0 &&
        options_.enable_auto_regularization) {
      StreamMessage(debug_level) <<
                                    "All poses active. Regularizing translation of root pose " <<
                                    root_pose_id_ << std::endl;

      Pose& root_pose = poses_[root_pose_id_];
      root_pose.is_param_mask_used = true;
      root_pose.param_mask.assign(kPoseDim, true);
      // dsiable the translation components.
      root_pose.param_mask[0] = root_pose.param_mask[1] =
          root_pose.param_mask[2] = false;

      if (kBiasInState && options_.regularize_biases_in_batch) {
        StreamMessage(debug_level) <<
                                      "Regularizing bias of first pose." << std::endl;
        root_pose.param_mask[9] = root_pose.param_mask[10] =
            root_pose.param_mask[11] = root_pose.param_mask[12] =
            root_pose.param_mask[13] = root_pose.param_mask[14] = false;
      }

      // if there is no velocity in the state, fix the three initial rotations,
      // as we don't need to accomodate gravity
      if (!kVelInState) {
        StreamMessage(debug_level) <<
                                      "Velocity not in state, regularizing rotation of root pose " <<
                                      root_pose_id_ << std::endl;

        root_pose.param_mask[3] = root_pose.param_mask[4] =
            root_pose.param_mask[5] = false;
      } else if (kGravityInCalib) {
        // If gravity is explicitly parameterized, fix the intial rotations
        root_pose.param_mask[3] = root_pose.param_mask[4] =
            root_pose.param_mask[5] = false;
      } else {
        // regularize one rotation axis due to gravity null space, depending on
        // the major gravity axis)
        uint32_t reg_dim = GetGravityRegularizationDimension(root_pose_id_);

        StreamMessage(debug_level) <<
                                      "Velocity in state. Regularizing dimension " << reg_dim << " of root "
                                                                                                 "pose rotation" << std::endl;

        root_pose.param_mask[reg_dim] = false;
      }
    }

    // used to store errors for robust norm calculation
    errors_.reserve(num_proj_res);
    errors_.clear();
    std::vector<Scalar> cond_errors;
    cond_errors.reserve(num_proj_res);

    StartTimer(_j_evaluation_);
    StartTimer(_j_evaluation_proj_);
    proj_error_ = 0;

    ParallelProjectionResiduals<BundleAdjuster<Scalar, LmSize, PoseSize,
        CalibSize, DoTvs>, Scalar> parallel_proj(*this);

    // tbb::parallel_reduce(tbb::blocked_range<int>(0, proj_residuals_.size()),
    //                      parallel_proj);
    parallel_proj(tbb::blocked_range<int>(0, proj_residuals_.size()));

    cond_errors = parallel_proj.cond_errors;
    errors_ = parallel_proj.errors;


    // get the sigma for robust norm calculation. This call is O(n) on average,
    // which is desirable over O(nlogn) sort
    if (errors_.size() > 0) {
      auto it = errors_.begin() + std::floor(errors_.size() * 0.5);
      std::nth_element(errors_.begin(), it, errors_.end());
      const Scalar sigma = sqrt(*it);

      Scalar cond_sigma = 0;
      if (cond_errors.size() > 0) {
        it = cond_errors.begin() + std::floor(cond_errors.size() * 0.5);
        std::nth_element(cond_errors.begin(), it, cond_errors.end());
        cond_sigma = sqrt(*it);
      }

      // std::cout << "Projection error sigma is " << dSigma << std::endl;
      // See "Parameter Estimation Techniques: A Tutorial with Application to
      // Conic Fitting" by Zhengyou Zhang. PP 26 defines this magic number:
      const Scalar c_huber = 1.2107 * sigma;
      const Scalar cond_c_huber = 1.2107 * cond_sigma;

      // now go through the measurements and assign weights
      for( ProjectionResidual& res : proj_residuals_ ){
        // calculate the huber norm weight for this measurement
        const Scalar e = sqrt(res.mahalanobis_distance);
        const bool use_robust = options_.use_robust_norm_for_proj_residuals; /*&&
          !res.is_conditioning;*/
        //!poses_[res.x_meas_id].is_active || !poses_[res.x_ref_id].is_active;
        const bool is_outlier =
            e > (res.is_conditioning ? cond_c_huber : c_huber);
        res.weight *= (is_outlier && use_robust ? c_huber/e : 1.0);
        res.mahalanobis_distance = res.residual.squaredNorm() * res.weight;
        r_pr_.template segment<ProjectionResidual::kResSize>(res.residual_offset) =
            res.residual * sqrt(res.weight);
        proj_error_ += res.mahalanobis_distance;
      }
    }
    errors_.clear();
    PrintTimer(_j_evaluation_proj_);

    StartTimer(_j_evaluation_binary_);
    binary_error_ = 0;
    // build binary residual jacobians
    for( BinaryResidual& res : binary_residuals_ ){
      const SE3t& t_w1 = poses_[res.x1_id].t_wp;
      const SE3t& t_w2 = poses_[res.x2_id].t_wp;
      const SE3t t_1w = t_w1.inverse();

      const Sophus::SE3Group<Scalar> t_12 = t_1w * t_w2;

      res.residual = res.cov_inv_sqrt * log_decoupled(t_12, res.t_12);

      const Eigen::Matrix<Scalar, 6, 7> dlog_dt1 =
          dLog_decoupled_dt1(t_12, res.t_12);

      res.dz_dx1 = dlog_dt1 * dt1_t2_dt1(t_1w, t_w2) *
          dinv_exp_decoupled_dx<Scalar>(t_w1);

      res.dz_dx2 = dlog_dt1 * dt1_t2_dt2(t_1w/*, t_w2*/) *
          dexp_decoupled_dx<Scalar>(t_w2);

      if (!res.use_rotation) {
        res.residual.template tail<3>().setZero();
        res.dz_dx1.template block<3, 6>(3, 0).setZero();
        res.dz_dx2.template block<3, 6>(3, 0).setZero();
      }

      BA_TEST(_Test_dBinaryResidual_dX(res, t_w1, t_w2));

      res.weight = res.orig_weight;
      r_pp_.template segment<BinaryResidual::kResSize>(res.residual_offset) =
          res.residual;

      res.mahalanobis_distance =
          (res.residual.transpose() * res.cov_inv * res.residual);
      binary_error_ +=  res.mahalanobis_distance * res.weight;
    }
    PrintTimer(_j_evaluation_binary_);

    StartTimer(_j_evaluation_unary_);
    unary_error_ = 0;
    errors_.clear();
    for( UnaryResidual& res : unary_residuals_ ){
      /*const */SE3t& t_wp = poses_[res.pose_id].t_wp;
      res.dz_dx = dlog_decoupled_dx(t_wp, res.t_wp);

      BA_TEST(_Test_dUnaryResidual_dX(res, t_wp));

      res.residual = log_decoupled(t_wp, res.t_wp);

      if (!res.use_rotation) {
        res.residual.template tail<3>().setZero();
        res.dz_dx.template block<3, 6>(3, 0).setZero();
      }

      res.weight = res.orig_weight;
      res.mahalanobis_distance =
          (res.residual.transpose() * res.cov_inv * res.residual);
      // this array is used to calculate the robust norm
      errors_.push_back(res.mahalanobis_distance);
      // r_u_.template segment<UnaryResidual::kResSize>(res.residual_offset) =
      //     res.residual;
      // unary_error_ += res.residual.transpose() * res.cov_inv * res.residual;
    }

    if (errors_.size() > 0) {
      auto it = errors_.begin()+std::floor(errors_.size()* 0.5);
      std::nth_element(errors_.begin(),it,errors_.end());
      const Scalar sigma = sqrt(*it);
      const Scalar c_huber = 1.2107 * sigma;
      // now go through the measurements and assign weights
      for( UnaryResidual& res : unary_residuals_ ){
        // calculate the huber norm weight for this measurement
        const Scalar e = sqrt(res.mahalanobis_distance);
        // We don't want to robust norm the conditioning edge
        const Scalar weight = ((e > c_huber) ? c_huber/e : 1.0);

        res.cov_inv = res.cov_inv * weight;
        res.cov_inv_sqrt = res.cov_inv.sqrt();
        decltype(res.residual) res_std_form = res.cov_inv_sqrt * res.residual;

        // now that we have the deltas with subtracted initial velocity,
        // transform and gravity, we can construct the jacobian
        r_u_.template segment<UnaryResidual::kResSize>(res.residual_offset) =
            res_std_form;
        // No need to multiply by sigma^-1 here, as the problem is in standard form.
        res.mahalanobis_distance =
            (res_std_form.transpose() * res_std_form);
        unary_error_ += res.mahalanobis_distance;
      }
    }
    errors_.clear();
    PrintTimer(_j_evaluation_unary_);

    errors_.reserve(num_im_res);
    errors_.clear();
    StartTimer(_j_evaluation_inertial_);
    inertial_error_ = 0;

    ParallelInertialResiduals<BundleAdjuster<Scalar, LmSize, PoseSize,
        CalibSize, DoTvs>, Scalar> parallel_in(*this);

    tbb::parallel_reduce(tbb::blocked_range<int>(
        0, inertial_residuals_.size()), parallel_in);

    errors_ = parallel_proj.errors;
    StartTimer(_j_evaluation_inertial_sqrt_);
    if (errors_.size() > 0) {
      auto it = errors_.begin()+std::floor(errors_.size()* 0.5);
      std::nth_element(errors_.begin(),it,errors_.end());
      const Scalar sigma = sqrt(*it);
      // std::cout << "Projection error sigma is " << dSigma << std::endl;
      // See "Parameter Estimation Techniques: A Tutorial with Application to
      // Conic Fitting" by Zhengyou Zhang. PP 26 defines this magic number:
      const Scalar c_huber = 1.2107*sigma;
      //std::cerr << "Sigma for imu errors: " << c_huber << " median res: " <<
      //             sigma <<  std::endl;

      // now go through the measurements and assign weights
      for( ImuResidual& res : inertial_residuals_ ){
        // Is this a conditioning edge?
        const bool use_robust = options_.use_robust_norm_for_inertial_residuals;
        const bool is_cond =
            !poses_[res.pose1_id].is_active && poses_[res.pose2_id].is_active;

        // calculate the huber norm weight for this measurement
        const Scalar e = sqrt(res.mahalanobis_distance);
        // We don't want to robust norm the conditioning edge
        const Scalar weight = ((e > c_huber) && !is_cond && use_robust ?
                                 c_huber/e : 1.0);

        // std::cerr << "Imu res " << res.residual_id << " error " <<
        //              e << " and huber w: " << weight << std::endl;

        res.cov_inv = res.cov_inv * weight;
        res.cov_inv_sqrt = res.cov_inv.sqrt();
        decltype(res.residual) res_std_form = res.cov_inv_sqrt * res.residual;

        // now that we have the deltas with subtracted initial velocity,
        // transform and gravity, we can construct the jacobian
        r_i_.template segment<ImuResidual::kResSize>(res.residual_offset) =
            res_std_form;
        // No need to multiply by sigma^-1 here, as the problem is in standard form.
        res.mahalanobis_distance =
            (res_std_form.transpose() * res_std_form);
        inertial_error_ += res.mahalanobis_distance;
      }
    }
    errors_.clear();
    PrintTimer(_j_evaluation_inertial_sqrt_);

    PrintTimer(_j_evaluation_inertial_);
    PrintTimer(_j_evaluation_);
    //TODO : The transpose insertions here are hideously expensive as they are
    // not in order. find a way around this.

    // here we sort the measurements and insert them per pose and per landmark,
    // this will mean each insert operation is O(1)
    //  dtime = Tic();

    StartTimer(_j_insertion_);
    StartTimer(_j_insertion_poses);
    // reserve space for j_pr_
    Eigen::VectorXi j_pr_sizes(num_poses);
    Eigen::VectorXi j_pp_sizes(num_poses);
    Eigen::VectorXi j_u_sizes(num_poses);
    Eigen::VectorXi j_i_sizes(num_poses);
    Eigen::VectorXi j_l_sizes(num_lm);

    for (Pose& pose : poses_) {
      if(pose.is_active){
        j_pr_sizes[pose.opt_id] = pose.proj_residuals.size();
        j_pp_sizes[pose.opt_id] = pose.binary_residuals.size();
        j_u_sizes[pose.opt_id] = pose.unary_residuals.size();
        j_i_sizes[pose.opt_id] = pose.inertial_residuals.size();
      }
    }

    for (Landmark& lm : landmarks_) {
      if (lm.is_active) {
        j_l_sizes[lm.opt_id] = lm.proj_residuals.size();
      }
    }

    StreamMessage(debug_level + 1) << "Reserving jacobians..." << std::endl;

    if (!proj_residuals_.empty() && num_poses > 0) {
      j_pr_.reserve(j_pr_sizes);
      jt_pr.reserve(Eigen::VectorXi::Constant(jt_pr.cols(),
                                              kLmDim == 1 ? 2 : 1));

      if (kJkprUsed) {
        j_kpr_.reserve(Eigen::VectorXi::Constant(1, num_proj_res));
        jt_kpr_.reserve(Eigen::VectorXi::Constant(num_proj_res, 1));
      }
    }

    if (!binary_residuals_.empty()) {
      j_pp_.reserve(j_pp_sizes);
      jt_pp_.reserve(Eigen::VectorXi::Constant(jt_pp_.cols(), 2));
    }

    if (!unary_residuals_.empty()) {
      j_u_.reserve(j_u_sizes);
      jt_u_.reserve(Eigen::VectorXi::Constant(jt_u_.cols(), 1));
    }

    if (!inertial_residuals_.empty()) {
      j_i_.reserve(j_i_sizes);
      jt_i_.reserve(Eigen::VectorXi::Constant(jt_i_.cols(), 2));

      if (kTvsInCalib) {
        j_ki_.reserve(Eigen::VectorXi::Constant(1, num_im_res));
        jt_ki_.reserve(Eigen::VectorXi::Constant(num_im_res, 1));
      }
    }

    if (num_lm > 0) {
      j_l_.reserve(j_l_sizes);
    }

    for (Pose& pose : poses_) {

      if (pose.is_active) {
        // sort the measurements by id so the sparse insert is O(1)
        std::sort(pose.proj_residuals.begin(), pose.proj_residuals.end());
        for (const int id: pose.proj_residuals) {
          ProjectionResidual& res = proj_residuals_[id];
          Eigen::Matrix<Scalar, 2, 6>& dz_dx =
              res.x_meas_id == pose.id ? res.dz_dx_meas : res.dz_dx_ref;
          if (pose.is_param_mask_used) {
            is_param_mask_used_ = true;
            for (uint32_t ii = 0 ; ii < kPrPoseDim ; ++ii) {
              if (!pose.param_mask[ii]) {
                dz_dx.col(ii).setZero();
              }
            }
          }

          // StreamMessage(debug_level) << "Inserting into " << res.residual_id <<
          //                               ", " << pose.opt_id << std::endl;
          // insert the jacobians into the sparse matrices
          // The weight is only multiplied by the transpose matrix, this is
          // so we can perform Jt*W*J*dx = Jt*W*r
          j_pr_.insert(
                res.residual_id, pose.opt_id).setZero().template block<2,6>(0,0) =
              dz_dx * sqrt(res.weight);

          jt_pr.insert(
                pose.opt_id, res.residual_id).setZero().template block<6,2>(0,0) =
              dz_dx.transpose() * sqrt(res.weight);
        }


        // add the pose/pose constraints
        std::sort(pose.binary_residuals.begin(), pose.binary_residuals.end());
        for (const int id: pose.binary_residuals) {
          BinaryResidual& res = binary_residuals_[id];
          Eigen::Matrix<Scalar,6,6>& dz_dz =
              res.x1_id == pose.id ? res.dz_dx1 : res.dz_dx2;

          if (pose.is_param_mask_used) {
            is_param_mask_used_ = true;
            for (int ii = 0 ; ii < 6 ; ++ii) {
              if (!pose.param_mask[ii]) {
                dz_dz.col(ii).setZero();
              }
            }
          }

          j_pp_.insert(
                res.residual_id, pose.opt_id ).setZero().template block<6,6>(0,0) =
              res.cov_inv_sqrt * dz_dz;

          jt_pp_.insert(
                pose.opt_id, res.residual_id ).setZero().template block<6,6>(0,0) =
              dz_dz.transpose() * res.cov_inv_sqrt * res.weight;
        }

        // add the unary constraints
        std::sort(pose.unary_residuals.begin(), pose.unary_residuals.end());
        for (const int id: pose.unary_residuals) {
          UnaryResidual& res = unary_residuals_[id];
          if (pose.is_param_mask_used) {
            is_param_mask_used_ = true;
            for (int ii = 0 ; ii < 6 ; ++ii) {
              if (!pose.param_mask[ii]) {
                res.dz_dx.col(ii).setZero();
              }
            }
          }
          j_u_.insert(
                res.residual_id, pose.opt_id ).setZero().template block<6,6>(0,0) =
              res.cov_inv_sqrt * res.dz_dx;

          jt_u_.insert(
                pose.opt_id, res.residual_id ).setZero().template block<6,6>(0,0) =
              res.dz_dx.transpose() * res.cov_inv_sqrt;
        }

        std::sort(pose.inertial_residuals.begin(), pose.inertial_residuals.end());
        for (const int id: pose.inertial_residuals) {
          ImuResidual& res = inertial_residuals_[id];
          Eigen::Matrix<Scalar,ImuResidual::kResSize,kPoseDim> dz_dz =
              res.pose1_id == pose.id ? res.dz_dx1 : res.dz_dx2;

          if (pose.is_param_mask_used) {
            is_param_mask_used_ = true;
            for (uint32_t ii = 0 ; ii < kPoseDim ; ++ii) {
              if (!pose.param_mask[ii]) {
                dz_dz.col(ii).setZero();
              }
            }
          }

          j_i_.insert(
                res.residual_id, pose.opt_id ) = res.cov_inv_sqrt * dz_dz;

          /*
        Eigen::Matrix<Scalar, ImuResidual::kResSize, ImuResidual::kResSize> sq =
          res.cov_inv_sqrt.eval();

        Eigen::Matrix<Scalar, kPoseDim, ImuResidual::kResSize> result =
            (trans * sq).eval(); ;
        jt_i_.insert(
          pose.opt_id, res.residual_id ) = result; */

          // ZZZZZZZ why is this necessary? Will this be a problem with the unary
          // residuals as well?
          const Eigen::Matrix<Scalar, kPoseDim, ImuResidual::kResSize> trans =
              dz_dz.transpose().eval();
          jt_i_.insert(
                pose.opt_id, res.residual_id ) = trans * res.cov_inv_sqrt;

        }
      }
    }
    PrintTimer(_j_insertion_poses);

    // fill in calibration jacobians
    StartTimer(_j_insertion_calib);
    if (kCalibDim > 0) {
      if (kGravityInCalib) {
        for (const ImuResidual& res : inertial_residuals_) {
          // include gravity terms (t total)
          if (kCalibDim > 0 ){
            Eigen::Matrix<Scalar,9,2> dz_dg = res.dz_dg;
            j_ki_.insert(res.residual_id, 0 ).setZero().
                template block(0,0,9,2) =
                res.cov_inv_sqrt * dz_dg.template block(0,0,9,2);

            // this down weights the velocity error
            dz_dg.template block<3,2>(6,0) *= 0.1;
            jt_ki_.insert( 0, res.residual_id ).setZero().
                template block(0,0,2,9) =
                dz_dg.transpose().template block(0,0,2,9) * res.cov_inv_sqrt;
          }
        }
      }

      // include imu to camera terms (6 total)
      if (kCamParamsInCalib) {
        for (const ProjectionResidual& res : proj_residuals_) {
          const Eigen::Matrix<Scalar,2, Eigen::Dynamic>& dz_dk =
              res.dz_dcam_params;

          const double weight_sqrt = sqrt(res.weight);
          j_kpr_.coeffRef(res.residual_id,0).setZero().
              template block(0, 0, 2, dz_dk.cols()) =
              dz_dk.template block(0, 0, 2, dz_dk.cols()) * weight_sqrt;

          jt_kpr_.coeffRef(0,res.residual_id).setZero().
              template block(0, 0, dz_dk.cols(), 2) =
              dz_dk.template block(0, 0, 2, dz_dk.cols()).transpose() * weight_sqrt;
        }
      }

      if (kTvsInCalib) {
        for (const ProjectionResidual& res : proj_residuals_) {
          const Eigen::Matrix<Scalar,2, 6>& dz_dk =
              res.dz_dtvs;

          const double weight_sqrt = sqrt(res.weight);
          j_kpr_.coeffRef(res.residual_id,0).setZero().
              template block(0, kTvsOffset, 2, dz_dk.cols()) =
              dz_dk.template block(0, 0, 2, dz_dk.cols()) * weight_sqrt;

          jt_kpr_.coeffRef(0,res.residual_id).setZero().
              template block(kTvsOffset, 0, dz_dk.cols(), 2) =
              dz_dk.template block(0, 0, 2, dz_dk.cols()).transpose() * weight_sqrt;
        }
      }
    }
    PrintTimer(_j_insertion_calib);

    StartTimer(_j_insertion_landmarks);
    for (Landmark& lm : landmarks_) {
      if (lm.is_active) {
        // sort the measurements by id so the sparse insert is O(1)
        std::sort(lm.proj_residuals.begin(), lm.proj_residuals.end());
        for (const int id: lm.proj_residuals) {
          const ProjectionResidual& res = proj_residuals_[id];

          j_l_.insert( res.residual_id, lm.opt_id ) = res.dz_dlm *
              sqrt(res.weight);
        }
      }
    }

    PrintTimer  (_j_insertion_landmarks);
    PrintTimer(_j_insertion_);
  }

  template<typename Scalar,int LmSize, int PoseSize, int CalibSize, bool DoTvs>
  double BundleAdjuster<Scalar, LmSize, PoseSize, CalibSize, DoTvs>::
  LandmarkOutlierRatio(const uint32_t id) const
  {
    return landmarks_[id].proj_residuals.size() == 0 ?
          0 : (double)landmarks_[id].num_outlier_residuals /
          landmarks_[id].proj_residuals.size();
  }

  // specializations
  // SelfCalBundleAdjuster<REAL_TYPE>
  template class BundleAdjuster<REAL_TYPE, 1, 6, 5>;
  // VisualBundleAdjuster<REAL_TYPE>
  template class BundleAdjuster<REAL_TYPE, 1, 6, 0>;
  // BundleAdjuster<Scalar, 1, 15, 0>
  template class BundleAdjuster<REAL_TYPE, 0, 6, 0>;
  template class BundleAdjuster<REAL_TYPE, 1, 15, 0>;
  template class BundleAdjuster<REAL_TYPE, 1, 15, 5, false>;
  template class BundleAdjuster<REAL_TYPE, 1, 15, 0, true>;
  template class BundleAdjuster<REAL_TYPE, 1, 15, 5, true>;
  template class BundleAdjuster<REAL_TYPE, 0, 15, 0, false>;


  // specializations required for the applications
#ifdef BUILD_APPS
  template class BundleAdjuster<double, 0,9,0>;
#endif
}
