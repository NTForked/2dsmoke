/*
 *  advect.cpp
 *  smoke
 *
 */

#include "advect.h"
#include "utility.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

const char *advection_name[] = { "Upwind", "WENO5", "QUICK", "Semi-Lagrangian", "MacCormack", NULL };
const char *interp_name[] = { "Linear", "Clamped Cubic Spline", "Monotinic Cubic", NULL };
const char *integrator_name[] = { "1st Order Euler", "2nd Order Modified Euler", "4th Order Runge-Kutta", NULL };

// Global References for Instance Access
static double ***gu = NULL;
static double **gc = NULL;
static int gn = 0;
static int gcn = 0;
static double (*interp_func)( double **d, int w, int h, double x, double y ) = NULL;

double monotonic_cubic_4( const double a[4], double x ) {
	
	double d0 = a[1] - a[0];
	double d1 = a[2] - a[1];
	double d2 = a[3] - a[2];

	if( ! d1 ) {
		d0 = d2 = 0.0;
	} else {
		double p = d1 > 0.0 ? 1.0 : -1.0;
		d0 = p*fabs(d0);
		d2 = p*fabs(d2);
	}

	double a3 = d2+d0;
	double a2 = -d2-2.0*d0;
	double a1 = d1+d0;
	double a0 = a[1];
	double y = a3*x*x*x+a2*x*x+a1*x+a0;
	return y;
}

double monotonic_cubic( double **d, int width, int height, double x, double y ) {
	double f[16];
	double xn[4];
	
	x = max(0.0,min(width,x));
	y = max(0.0,min(height,y));
	
	for( int j=0; j<4; j++ ) {
		for( int i=0; i<4; i++ ) {
			int h = (int)x - 1 + i;
			int v = (int)y - 1 + j;
			f[4*j+i] = d[min(width-1,max(0,h))][min(height-1,max(0,v))];
		}
	}
	
	for( int j=0; j<4; j++ ) {
		xn[j] = monotonic_cubic_4( &f[4*j], x - (int)x );
	}
	
	return monotonic_cubic_4( xn, y - (int)y );
}

double spline_cubic(const double a[4], double x) {
	int i, j;
	double alpha[4], l[4], mu[4], z[4];
	double b[4], c[4], d[4];
	for(i = 1; i < 3; i++) {
		alpha[i] = 3.0 * (a[i+1] - a[i]) - 3.0 * (a[i] - a[i-1]);
	}
	l[0] = 1.0;
	mu[0] = 0.0;
	z[0] = 0.0;
	for(i = 1; i < 3; i++) {
		l[i] = 4.0 - mu[i-1];
		mu[i] = 1.0 / l[i];
		z[i] = (alpha[i] - z[i-1]) / l[i];
	}
	l[3] = 1.0;
	z[3] = 0.0;
	c[3] = 0.0;
	for(j = 2; 0 <= j; j--) {
		c[j] = z[j] - mu[j] * c[j+1];
		b[j] = a[j+1] - a[j] - (c[j+1] + 2.0 * c[j]) / 3.0;
		d[j] = (c[j+1] - c[j]) / 3.0;
	}
	
	double minv = min(a[1],a[2]);
	double maxv = max(a[2],a[1]);
	return min(maxv,max(minv,(a[1] + b[1] * x + c[1] * x * x + d[1] * x * x * x )));
}

double spline_interpolate( double **d, int width, int height, double x, double y ) {
	double f[16];
	double xn[4];
	
	x = max(0.0,min(width,x));
	y = max(0.0,min(height,y));
	
	for( int j=0; j<4; j++ ) {
		for( int i=0; i<4; i++ ) {
			int h = (int)x - 1 + i;
			int v = (int)y - 1 + j;
			f[4*j+i] = d[min(width-1,max(0,h))][min(height-1,max(0,v))];
		}
	}
	
	for( int j=0; j<4; j++ ) {
		xn[j] = spline_cubic( &f[4*j], x - (int)x );
	}
	
	return spline_cubic( xn, y - (int)y );
}

static double linear_interpolate ( double **d, int width, int height, double x, double y ) {
	x = max(0.0,min(width,x));
	y = max(0.0,min(height,y));
	int i = min(x,width-2);
	int j = min(y,height-2);
	
	return ((i+1-x)*d[i][j]+(x-i)*d[i+1][j])*(j+1-y) + ((i+1-x)*d[i][j+1]+(x-i)*d[i+1][j+1])*(y-j);
}

inline double square(double x) {
	return x*x;
}

// WENO5 Advection
static double weno5calc( double v1, double v2, double v3, double v4, double v5 ) {
	double e = 1.0e-6;
	double r1 = 13.0 * square(v1-2.0*v2+v3) / 12.0 + square(v1-4.0*v2+3.0*v3) / 4.0;
	double r2 = 13.0 * square(v2-2.0*v3+v4) / 12.0 + square(v2-v4) / 4.0;
	double r3 = 13.0 * square(v3-2.0*v4+v5) / 12.0 + square(3.0*v3-4.0*v4+v5) / 4.0;
	double w1 = 0.1 / square(e+r1);
	double w2 = 0.6 / square(e+r2);
	double w3 = 0.3 / square(e+r3);
	double s = w1+w2+w3;
	w1 = w1 / s;
	w2 = w2 / s;
	w3 = w3 / s;
	return (w1*(2.0*v1-7.0*v2+11.0*v3)+w2*(-v2+5.0*v3+2.0*v4)+w3*(2.0*v3+5.0*v4-v5))/6.0;
}
static double weno5( double u, double d0, double d1, double d2, double d3, double d4, double d5, double d6) {
	return -u*((u>0)*weno5calc(d1-d0,d2-d1,d3-d2,d4-d3,d5-d4)+(u<0)*weno5calc(d6-d5,d5-d4,d4-d3,d3-d2,d2-d1));
}

static double QUICK( double u, double d0, double d1, double d2, double d3, double d4 ) {
	double center = 0.5*(d3-d1);
	return -u*(center+(u>0)*(d4-3.0*d3+3.0*d2-d1)/8.0 + (u<0)*(d3-3.0*d2+3.0*d1-d0)/8.0);
}

// Clamped Fluid Flow Fetch
static double u_ref( int dir, int i, int j ) {
	if( dir == 0 )
		return gu[0][max(0,min(gn,i))][max(0,min(gn-1,j))];
	else
		return gu[1][max(0,min(gn-1,i))][max(0,min(gn,j))];
}

// Clamped Concentration Fetch
static double c_ref( int i, int j ) {
	if( i < 0 || i > gcn-1 || j < 0 || j > gcn-1 ) return 0.0;
	return gc[i][j];
}

// 1D Derivative Advection
static double advdiff( int method, double u, double d0, double d1, double d2, double d3, double d4, double d5, double d6) {
	double dd = 0.0;
	switch( method ) {
		case 0: // Upwind
			dd = -u*((u>0)*(d3-d2)+(u<0)*(d4-d3));
			break;
		case 1: // WENO5
			dd = weno5(u,d0,d1,d2,d3,d4,d5,d6);
			break;
		case 2: // QUICK
			dd = QUICK(u,d1,d2,d3,d4,d5);
			break;
	}
	return dd;
}

// 2D Derivative Advection
static void advect_diff( int method, double ***u, double **c, int n, int cn, double **out[3], double dt ) {
	static double **up[2] = { alloc2D(n), alloc2D(n) };
	
	// Advect X Flow
	OPENMP_FOR FOR_EVERY_X_FLOW(n) {
		double v[2] = { u[0][i][j], (u_ref(1,i-1,j)+u_ref(1,i,j)+u_ref(1,i-1,j+1)+u_ref(1,i,j+1))/4.0 };
		
		out[0][i][j] = 0.0;
		
		// X Direction
		out[0][i][j] += advdiff( method, v[0], u_ref(0,i-3,j), u_ref(0,i-2,j), u_ref(0,i-1,j), u_ref(0,i,j), 
								u_ref(0,i+1,j), u_ref(0,i+2,j), u_ref(0,i+3,j) ) * n;
		
		// Y Direction
		out[0][i][j] += advdiff( method, v[1], u_ref(0,i,j-3), u_ref(0,i,j-2), u_ref(0,i,j-1), u_ref(0,i,j), 
								u_ref(0,i,j+1), u_ref(0,i,j+2), u_ref(0,i,j+3) ) * n;
	} END_FOR
	
	// Advect Y Flow
	OPENMP_FOR FOR_EVERY_Y_FLOW(n) {
		double v[2] = { (u_ref(0,i,j-1)+u_ref(0,i,j)+u_ref(0,i+1,j)+u_ref(0,i+1,j-1))/4.0, u[1][i][j] };
		
		out[1][i][j] = 0.0;
		
		// X Direction
		out[1][i][j] += advdiff( method, v[0], u_ref(1,i-3,j), u_ref(1,i-2,j), u_ref(1,i-1,j), u_ref(1,i,j), 
								u_ref(1,i+1,j), u_ref(1,i+2,j), u_ref(1,i+3,j) ) * n;
		
		// Y Direction
		out[1][i][j] += advdiff( method, v[1], u_ref(1,i,j-3), u_ref(1,i,j-2), u_ref(1,i,j-1), u_ref(1,i,j), 
								u_ref(1,i,j+1), u_ref(1,i,j+2), u_ref(1,i,j+3) ) * n;
		
	} END_FOR
	
	// Advect Concentration
	OPENMP_FOR FOR_EVERY_CELL(n) {
		up[0][i][j] = 0.5*u[0][i][j]+0.5*u[0][i+1][j];
		up[1][i][j] = 0.5*u[1][i][j]+0.5*u[1][i][j+1];
	} END_FOR
	
	OPENMP_FOR FOR_EVERY_CELL(cn) {
		double x = i*n/(double)cn;
		double y = j*n/(double)cn;
		double v[2] = { linear_interpolate( up[0], n, n, x, y ), linear_interpolate( up[1], n, n, x, y ) };

		out[2][i][j] = 0.0;
		
		// X Direction
		out[2][i][j] += advdiff( method, v[0], c_ref(i-3,j), c_ref(i-2,j), c_ref(i-1,j), c_ref(i,j), 
								c_ref(i+1,j), c_ref(i+2,j), c_ref(i+3,j) ) * cn;
		
		// Y Direction
		out[2][i][j] += advdiff( method, v[1], c_ref(i,j-3), c_ref(i,j-2), c_ref(i,j-1), c_ref(i,j), 
								c_ref(i,j+1), c_ref(i,j+2), c_ref(i,j+3) ) * cn;
		
	} END_FOR
}

static void maccormack ( double **d, double **d0, int width, int height, double ***u, float dt )
{
	OPENMP_FOR
	for( int n=0; n<width*height; n++ ) {
		int i = n%width;
		int j = n/width;
		
		double x = min(width-1,max(0.0,i-dt*gn*u[0][i][j]));
		double y = min(height-1,max(0.0,j-dt*gn*u[1][i][j]));
		
		int i0 = min(width-2,max(0,(int)x));
		int j0 = min(height-2,max(0,(int)y));
		
		int i1 = i0+1;
		int j1 = j0+1;
		
		double phi_n_1_hat = interp_func( d0, width, height, x, y );
		double u_hat = interp_func( u[0], width, height, x, y );
		double v_hat = interp_func( u[1], width, height, x, y );
		
		x += dt*gn*u_hat;
		y += dt*gn*v_hat;
		
		double phi_n_hat = interp_func( d0, width, height, x, y );
		
		double min_phi = min( min( min( d0[i0][j0], d0[i1][j0] ), d0[i0][j1] ), d0[i1][j1] );
		double max_phi = max( max( max( d0[i0][j0], d0[i1][j0] ), d0[i0][j1] ), d0[i1][j1] );
		double r = phi_n_1_hat + 0.5*( d0[i][j] - phi_n_hat);
		
		d[i][j] = max( min(r, max_phi), min_phi );
	}
}

static void semiLagrangian( double **d, double **d0, int width, int height, double ***u, float dt ) {
	OPENMP_FOR
	for( int n=0; n<width*height; n++ ) {
		int i = n%width;
		int j = n/width;
		d[i][j] = interp_func( d0, width, height, i-gn*u[0][i][j]*dt, j-gn*u[1][i][j]*dt );
	}
}

// Semi-Lagrangian Advection Method
static void advect_semiLagrangian( int method, double ***u, double **c, int n, int cn, double **out[3], double dt ) {
	
	double h = 1.0/n;
	double ch = 1.0/cn;
	
	// BackTrace Order
	int order = 1;
	
	// Set Strategy
	switch(method) {
		case 3:
			// Semi-Lagrangian Advection
			order = 1;
			break;
		case 4:
			// 2nd Order MacCormack Advection
			order = 2;
			interp_func = spline_interpolate;
			break;
	}
	
	// Compute Fluid Velocity At Each Staggered Faces And Concentration Cell Centers
	static double **ux[2] = { alloc2D(n+1), alloc2D(n+1) };
	static double **uy[2] = { alloc2D(n+1), alloc2D(n+1) };
	static double **up[2] = { alloc2D(n), alloc2D(n) };
	static double **uc[2] = { alloc2D(cn), alloc2D(cn) };
	
	OPENMP_FOR FOR_EVERY_X_FLOW(n) {
		ux[0][i][j] = u[0][i][j];
		ux[1][i][j] = (u_ref(1,i-1,j)+u_ref(1,i,j)+u_ref(1,i-1,j+1)+u_ref(1,i,j+1))/4.0;
	} END_FOR
	
	OPENMP_FOR FOR_EVERY_Y_FLOW(n) {
		uy[0][i][j] = (u_ref(0,i,j-1)+u_ref(0,i,j)+u_ref(0,i+1,j)+u_ref(0,i+1,j-1))/4.0;
		uy[1][i][j] = u[1][i][j];
	} END_FOR
	
	OPENMP_FOR FOR_EVERY_CELL(n) {
		up[0][i][j] = 0.5*u[0][i][j]+0.5*u[0][i+1][j];
		up[1][i][j] = 0.5*u[1][i][j]+0.5*u[1][i][j+1];
	} END_FOR
	
	OPENMP_FOR FOR_EVERY_CELL(cn) {
		double x = i*n/(double)cn;
		double y = j*n/(double)cn;
		uc[0][i][j] = interp_func( up[0], n, n, x, y );
		uc[1][i][j] = interp_func( up[1], n, n, x, y );
	} END_FOR

	// 1st Order Semi Advection
	if( order == 1 ) {
		// BackTrace X Flow
		semiLagrangian( out[0], u[0], n+1, n, ux, dt );

		// BackTrace Y Flow
		semiLagrangian( out[1], u[1], n, n+1, uy, dt );
			
		// BackTrace Concentration
		semiLagrangian( out[2], c, cn, cn, uc, dt );
		
	// 2nd Order MacCormack Method
	} else if( order == 2 ) {
		// BackTrace X Flow
		maccormack( out[0], u[0], n+1, n, ux, dt );
		
		// BackTrace Y Flow
		maccormack( out[1], u[1], n, n+1, uy, dt );
		
		// BackTrace Concentration
		maccormack( out[2], c, cn, cn, uc, dt );	
	}
}

static double advect_step( int method, double ***u, double **c, int n, int cn, double ***out, double dt ) {
	if( method < 3 ) { 
		// Upwind or WENO5
		advect_diff(method,u,c,n,cn,out,dt); // Watch for a CFL condition
	} else {
		// Semi-lagrangian
		advect_semiLagrangian(method,u,c,n,cn,out,dt);
	}
}

void advect::advect( int method, int interp, int integrator, double ***u, double **c, int n, int cn, double dt ) {
	
	gu = u;
	gc = c;
	gn = n;
	gcn = cn;
	
	double h = 1.0/n;
	double ch = 1.0/cn;
	
	// Memory Allocation
	static double **k[4][3] = { NULL, NULL, NULL, NULL };
	static double **tmp[3] = { alloc2D(n+1), alloc2D(n+1), alloc2D(cn) };
	for( int kn=0; kn<4; kn++ ) {
		if( ! k[kn][0] ) k[kn][0] = alloc2D(n+1);
		if( ! k[kn][1] ) k[kn][1] = alloc2D(n+1);
		if( ! k[kn][2] ) k[kn][2] = alloc2D(cn);
	}
	
	// Set Interpolation Method
	if( interp == 0 ) {
		interp_func = linear_interpolate;
	} else if(interp == 1)  {
		interp_func = spline_interpolate;
	} else {
		interp_func = monotonic_cubic;
	}
	
	if( method < 3 ) {
		if( integrator == 0 ) {			// Forward Euler Method
			advect_step( method, u, c, n, cn, k[0], dt );
			
			op2D(u[0],u[0],k[0][0],1.0,dt,n+1);
			op2D(u[1],u[1],k[0][1],1.0,dt,n+1);
			op2D(c,c,k[0][2],1.0,dt,cn);
			
		} else if( integrator == 1 ) { // Modified Euler Method
			// k0 = f'(x)
			advect_step( method, u, c, n, cn, k[0], dt );
			
			// k1 = f'(x + k0*dt)
			op2D(tmp[0], u[0], k[0][0], 1.0, dt, n+1);
			op2D(tmp[1], u[1], k[0][1], 1.0, dt, n+1);
			op2D(tmp[2], c,    k[0][2], 0.5, dt, cn);
			advect_step( method, tmp, tmp[2], n, cn, k[1], dt );
			
			// y = x + 0.5*dt*(k0+k1)
			op2D(u[0], u[0], k[0][0], 1.0, 0.5*dt, n+1 );
			op2D(u[1], u[1], k[0][1], 1.0, 0.5*dt, n+1 );
			op2D(c,    c,    k[0][2], 1.0, 0.5*dt, cn  );
			
			op2D(u[0], u[0], k[1][0], 1.0, 0.5*dt, n+1 );
			op2D(u[1], u[1], k[1][1], 1.0, 0.5*dt, n+1 );
			op2D(c,    c,    k[1][2], 1.0, 0.5*dt, cn  );
			
		} else if( integrator == 2  ) { // Runge-Kutta Method
			// k0 = f'(x)
			advect_step( method, u, c, n, cn, k[0], dt );
			
			// k1 = f'(x + 0.5*k0*dt)
			op2D(tmp[0], u[0], k[0][0], 1.0, 0.5*dt, n+1);
			op2D(tmp[1], u[1], k[0][1], 1.0, 0.5*dt, n+1);
			op2D(tmp[2], c,    k[0][2], 0.5, 0.5*dt, cn);
			advect_step( method, tmp, tmp[2], n, cn, k[1], dt );
			
			// k2 = f'(x + 0.5*k1*dt)
			op2D(tmp[0], u[0], k[1][0], 1.0, 0.5*dt, n+1);
			op2D(tmp[1], u[1], k[1][1], 1.0, 0.5*dt, n+1);
			op2D(tmp[2], c,    k[1][2], 0.5, 0.5*dt, cn);
			advect_step( method, tmp, tmp[2], n, cn, k[2], dt );
			
			// k3 = f'(x + 0.5*k2*dt)
			op2D(tmp[0], u[0], k[2][0], 1.0, dt, n+1);
			op2D(tmp[1], u[1], k[2][1], 1.0, dt, n+1);
			op2D(tmp[2], c,    k[2][2], 0.5, dt, cn);
			advect_step( method, tmp, tmp[2], n, cn, k[3], dt );
			
			// y = x + dt*(k0+2*k1+2*k2+k3)/6
			op2D(u[0], u[0], k[0][0], 1.0, dt/6.0, n+1 );
			op2D(u[1], u[1], k[0][1], 1.0, dt/6.0, n+1 );
			op2D(c,    c,    k[0][2], 1.0, dt/6.0, cn  );
			
			op2D(u[0], u[0], k[1][0], 1.0, dt/3.0, n+1 );
			op2D(u[1], u[1], k[1][1], 1.0, dt/3.0, n+1 );
			op2D(c,    c,    k[1][2], 1.0, dt/3.0, cn  );
			
			op2D(u[0], u[0], k[2][0], 1.0, dt/3.0, n+1 );
			op2D(u[1], u[1], k[2][1], 1.0, dt/3.0, n+1 );
			op2D(c,    c,    k[2][2], 1.0, dt/3.0, cn  );
			
			op2D(u[0], u[0], k[3][0], 1.0, dt/6.0, n+1 );
			op2D(u[1], u[1], k[3][1], 1.0, dt/6.0, n+1 );
			op2D(c,    c,    k[3][2], 1.0, dt/6.0, cn  );
		}
	} else {
		advect_step( method, u, c, n, cn, k[0], dt );
		copy2D(u[0],k[0][0],n+1);
		copy2D(u[1],k[0][1],n+1);
		copy2D(c,k[0][2],cn);
	}
}



















