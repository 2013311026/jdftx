/*-------------------------------------------------------------------
Copyright 2014 Ravishankar Sundararaman

This file is part of JDFTx.

JDFTx is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

JDFTx is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with JDFTx.  If not, see <http://www.gnu.org/licenses/>.
-------------------------------------------------------------------*/

#include <wannier/WannierMinimizerRS.h>
#include <core/WignerSeitz.h>

inline void setRealSpaceMeasures(size_t iStart, size_t iStop, const vector3<int>& S, const matrix3<>& R, const WignerSeitz* ws, vector3<double*> r, double* rSq)
{	vector3<> invS(1./S[0], 1./S[1], 1./S[2]);
	THREAD_rLoop
	(	vector3<> x; for(int k=0; k<3; k++) x[k] = iv[k] * invS[k]; //lattice coordinates
		x = ws->restrict(x); //wrap to Wigner-Seitz cell
		vector3<> rVec = ws->boundaryDistance(x) ? R * x : vector3<>(); //convert to Cartesian (and zero boundaries)
		storeVector(rVec, r, i);
		rSq[i] = rVec.length_squared();
	)
}

WannierMinimizerRS::WannierMinimizerRS(const Everything& e, const Wannier& wannier)
: WannierMinimizer(e, wannier, true)
{
	//Initialize the r and r^2 scalar fields:
	nullToZero(r, gInfoSuper);
	nullToZero(rSq, gInfoSuper);
	WignerSeitz ws(gInfoSuper.R);
	threadLaunch(setRealSpaceMeasures, gInfoSuper.nr, gInfoSuper.S, gInfoSuper.R, &ws, r.data(), rSq->dataPref());
	
	//Split centers over MPI processes:
	nStart = (nCenters * mpiUtil->iProcess()) / mpiUtil->nProcesses();
	nStop = (nCenters * (mpiUtil->iProcess()+1)) / mpiUtil->nProcesses();
}

void WannierMinimizerRS::initialize(int iSpin)
{	this->iSpin = iSpin;
}

static complexDataRptr operator*(complex s, const complexDataRptr& in)
{	complexDataRptr out = clone(in);
	callPref(eblas_zscal)(out->nElem, s, out->dataPref(),1);
	return out;
}

double WannierMinimizerRS::getOmega(bool grad, bool invariant)
{	resumeOperatorThreading();
	//Compute supercell wave-functions:
	ColumnBundle Csuper(nCenters, basisSuper.nbasis, &basisSuper, &qnumSuper, isGpuEnabled());
	Csuper.zero();
	for(unsigned i=0; i<kMesh.size(); i++) if(isMine_q(i,iSpin))
	{	const KmeshEntry& ki = kMesh[i];
		axpyWfns(ki.point.weight, ki.U, ki.point, iSpin, Csuper);
	}
	Csuper.allReduce(MPIUtil::ReduceSum);
	
	//Compute spread (and optionally its gradient w.r.t Csuper):
	double Omega = 0.;
	rSqExpect.assign(nCenters, 0.);
	rExpect.assign(nCenters, vector3<>());
	ColumnBundle Omega_Csuper; if(grad) { Omega_Csuper = Csuper.similar(); Omega_Csuper.zero(); }
	for(int n=nStart; n<nStop; n++)
	{	complexDataRptr psi = I(Csuper.getColumn(n)), Omega_psi;
		rSqExpect[n] = gInfoSuper.dV * dot(psi, rSq*psi).real();
		if(grad) Omega_psi += (2*gInfoSuper.dV) * (rSq*psi);
		for(int dir=0; dir<3; dir++)
		{	rExpect[n][dir] = gInfoSuper.dV * dot(psi, r[dir]* psi).real();
			if(grad) Omega_psi += (-2.*rExpect[n][dir] * 2*gInfoSuper.dV) * r[dir]*psi;
		}
		Omega += (rSqExpect[n] - rExpect[n].length_squared());
		//Off-diagonal corrections to get the invariant part (expensive):
		if(invariant)
		{	for(int m=0; m<nCenters; m++) if(m != n)
			{	complexDataRptr psi_m = I(Csuper.getColumn(m)), Omega_psi_m;
				for(int dir=0; dir<3; dir++)
				{	complex rExpect_mn = gInfoSuper.dV * dot(psi_m, r[dir]* psi);
					Omega -= rExpect_mn.norm();
					if(grad)
					{	Omega_psi -= 2*rExpect_mn * gInfoSuper.dV * (r[dir] * psi_m);
						Omega_psi_m -= 2*rExpect_mn.conj() * gInfoSuper.dV * (r[dir] * psi);
					}
				}
				if(grad && Omega_psi_m) Omega_Csuper.accumColumn(m, Idag(Omega_psi_m));
			}
		}
		if(grad) Omega_Csuper.accumColumn(n, Idag(Omega_psi));
	}
	mpiUtil->allReduce(Omega, MPIUtil::ReduceSum);
	mpiUtil->allReduce(rSqExpect.data(), nCenters, MPIUtil::ReduceSum);
	mpiUtil->allReduce((double*)rExpect.data(), 3*nCenters, MPIUtil::ReduceSum);
	
	if(grad)
	{	//Collect across processes:
		Omega_Csuper.allReduce(MPIUtil::ReduceSum);
		//Propagate to rotations:
		for(unsigned i=0; i<kMesh.size(); i++) if(isMine_q(i,iSpin))
		{	KmeshEntry& ki = kMesh[i];
			axpyWfns_grad(ki.point.weight, ki.Omega_U, ki.point, iSpin, Omega_Csuper);
		}
	}
	suspendOperatorThreading();
	return Omega;
}

double WannierMinimizerRS::getOmega(bool grad)
{	return getOmega(grad, false);
}

double WannierMinimizerRS::getOmegaI(bool grad)
{	return getOmega(grad, true);
}