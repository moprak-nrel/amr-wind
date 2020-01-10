#include <incflo.H>

#include <cmath>
#include <limits>

using namespace amrex;

//
// Compute new dt by using the formula derived in
// "A Boundary Condition Capturing Method for Multiphase Incompressible Flow"
// by Kang et al. (JCP).
//
//  dt/2 * ( C+V + sqrt( (C+V)**2 + 4Fx/dx + 4Fy/dy + 4Fz/dz )
//
// where
//
// C = max(|U|)/dx + max(|V|)/dy + max(|W|)/dz    --> Convection
//
// V = 2 * max(eta/rho) * (1/dx^2 + 1/dy^2 +1/dz^2) --> Diffusion
//
// Fx, Fy, Fz = net acceleration due to external forces
//
// WARNING: We use a slightly modified version of C in the implementation below
//
void incflo::ComputeDt (int initialization, bool explicit_diffusion)
{
    BL_PROFILE("incflo::ComputeDt");

    // Store the past two dt
    prev_prev_dt = prev_dt;
    prev_dt = dt;

    Real conv_cfl = 0.0;
    Real diff_cfl = 0.0;
    for (int lev = 0; lev <= finest_level; ++lev)
    {
        auto const dxinv = geom[lev].InvCellSizeArray();
        MultiFab const& vel = m_leveldata[lev]->velocity;
        MultiFab const& rho = m_leveldata[lev]->density;
        Real conv_lev = 0.0;
        Real diff_lev = 0.0;
#ifdef AMREX_USE_EB
        if (!vel.isAllRegular()) {
            auto const& flag = EBFactory(lev).getMultiEBCellFlagFab();
            conv_lev = amrex::ReduceMax(vel, flag, 0,
                       [=] AMREX_GPU_HOST_DEVICE (Box const& b,
                                                  Array4<Real const> const& v,
                                                  Array4<EBCellFlag const> const& f) -> Real
                       {
                           Real mx = -1.0;
                           amrex::Loop(b, [=,&mx] (int i, int j, int k) noexcept
                           {
                               if (!f(i,j,k).isCovered()) {
                                   mx = amrex::max(std::abs(v(i,j,k,0))*dxinv[0],
                                                   std::abs(v(i,j,k,1))*dxinv[1],
                                                   std::abs(v(i,j,k,2))*dxinv[2], mx);
                               }
                           });
                           return mx;
                       });
            if (explicit_diffusion) {
                diff_lev = amrex::ReduceMax(rho, flag, 0,
                           [=] AMREX_GPU_HOST_DEVICE (Box const& b,
                                                      Array4<Real const> const& r,
                                                      Array4<EBCellFlag const> const& f) -> Real
                          {
                              Real mx = -1.0;
                              amrex::Loop(b, [=,&mx] (int i, int j, int k) noexcept
                              {
                                  if (!f(i,j,k).isCovered()) {
                                      mx = amrex::max(1.0/r(i,j,k), mx);
                                  }
                              });
                              return mx;
                          });
                diff_lev *= this->mu;
            }
        } else
#endif
        {
            conv_lev = amrex::ReduceMax(vel, 0,
                       [=] AMREX_GPU_HOST_DEVICE (Box const& b,
                                                  Array4<Real const> const& v) -> Real
                       {
                           Real mx = -1.0;
                           amrex::Loop(b, [=,&mx] (int i, int j, int k) noexcept
                           {
                               mx = amrex::max(std::abs(v(i,j,k,0))*dxinv[0],
                                               std::abs(v(i,j,k,1))*dxinv[1],
                                               std::abs(v(i,j,k,2))*dxinv[2], mx);
                           });
                           return mx;
                       });
            if (explicit_diffusion) {
                diff_lev = amrex::ReduceMax(rho, 0,
                           [=] AMREX_GPU_HOST_DEVICE (Box const& b,
                                                      Array4<Real const> const& r) -> Real
                           {
                               Real mx = -1.0;
                               amrex::Loop(b, [=,&mx] (int i, int j, int k) noexcept
                               {
                                   mx = amrex::max(1.0/r(i,j,k), mx);
                               });
                               return mx;
                           });
                diff_lev *= this->mu;
            }
        }
        conv_cfl = std::max(conv_cfl, conv_lev);
        diff_cfl = std::max(diff_cfl, diff_lev*2.*(dxinv[0]*dxinv[0]+dxinv[1]*dxinv[1]+
                                                   dxinv[2]*dxinv[2]));
    }

    Real cd_cfl;
    if (explicit_diffusion) {
        ParallelAllReduce::Max<Real>({conv_cfl,diff_cfl},
                                     ParallelContext::CommunicatorSub());
        cd_cfl = conv_cfl + diff_cfl;
    } else {
        ParallelAllReduce::Max<Real>(conv_cfl,
                                     ParallelContext::CommunicatorSub());
        cd_cfl = conv_cfl;
    }

    // Forcing term
    const auto dxinv_finest = Geom(finest_level).InvCellSizeArray();
    Real forc_cfl = std::abs(gravity[0] - std::abs(gp0[0])) * dxinv_finest[0]
                  + std::abs(gravity[1] - std::abs(gp0[1])) * dxinv_finest[1]
                  + std::abs(gravity[2] - std::abs(gp0[2])) * dxinv_finest[2];

    // Combined CFL conditioner
    Real comb_cfl = cd_cfl + std::sqrt(cd_cfl*cd_cfl + 4.0 * forc_cfl);

    // Update dt
    Real dt_new = 2.0 * cfl / comb_cfl;

    // Optionally reduce CFL for initial step
    if(initialization)
    {
        dt_new *= init_shrink;
    }

    // Protect against very small comb_cfl
    // This may happen, for example, when the initial velocity field
    // is zero for an inviscid flow with no external forcing
    Real eps = std::numeric_limits<Real>::epsilon();
    if(comb_cfl <= eps)
    {
        dt_new = 0.5 * dt;
    }

    // Don't let the timestep grow by more than 10% per step 
    // unless the previous time step was unduly shrunk to match plot_per_exact
    if( (dt > 0.0) && !(plot_per_exact > 0 && last_plt == nstep && nstep > 0) )
    {
        dt_new = amrex::min(dt_new, 1.1 * prev_dt);
    } 
    else if ( (dt > 0.0) && (plot_per_exact > 0 && last_plt == nstep && nstep > 0) )
    {
        dt_new = amrex::min( dt_new, 1.1 * amrex::max(prev_dt, prev_prev_dt) );
    }
    
    // Don't overshoot specified plot times
    if(plot_per_exact > 0.0 && 
            (std::trunc((cur_time + dt_new + eps) / plot_per_exact) > std::trunc((cur_time + eps) / plot_per_exact)))
    {
        dt_new = std::trunc((cur_time + dt_new) / plot_per_exact) * plot_per_exact - cur_time;
    }

    // Don't overshoot the final time if not running to steady state
    if(!steady_state && stop_time > 0.0)
    {
        if(cur_time + dt_new > stop_time)
        {
            dt_new = stop_time - cur_time;
        }
    }

    // Make sure the timestep is not set to zero after a plot_per_exact stop
    if(dt_new < eps)
    {
        dt_new = 0.5 * dt;
    }

    // If using fixed time step, check CFL condition and give warning if not satisfied
    if(fixed_dt > 0.0)
    {
	if(dt_new < fixed_dt)
	{
		amrex::Print() << "WARNING: fixed_dt does not satisfy CFL condition: \n"
					   << "max dt by CFL     : " << dt_new << "\n"
					   << "fixed dt specified: " << fixed_dt << std::endl;
	}
	dt = fixed_dt;
    }
    else
    {
	dt = dt_new;
    }
}
