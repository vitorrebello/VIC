#include <stdio.h>
#include <stdlib.h>
#include <vicNl.h>
#include <string.h>

static char vcid[] = "$Id$";

void initialize_model_state(double               surf_temp, 
			    int                  Ndist,
                            int                  Nnodes,
		            int                  Nveg,
			    int                  cellnum,
			    dist_prcp_struct    *prcp,
			    dmy_struct           dmy,
                            global_param_struct *global_param,
                            infiles_struct       infiles,
#if LAKE_MODEL
			    lake_con_struct lake_con,
#endif //LAKE_MODEL
                            soil_con_struct     *soil_con,
			    veg_con_struct      *veg_con)
/**********************************************************************
  initialize_model_state       Keith Cherkauer	    April 17, 2000

  This routine initializes the model state (energy balance, water balance,
  and snow components).  If a state file is provided to the model than its
  contents are checked to see if it agrees with the current simulation
  set-up, if so it is used to initialize the model state.  If no state
  file is provided the model initializes all variables with defaults and
  the user should expect to throw out the beginning of the simulation 
  period as model start-up.

  UNITS: (m, s, kg, C, moisture in mm) unless otherwise specified

  Modifications:
  4-17-00 Modified from initialize_energy_bal.c and initialize_snow.c
          to provide a single controlling routine for initializing the
          model state.
  9-00    Fixed bug where initialization of soil node temperatures 
          and moitures was within two vegetation loops, thus only
          the first vegetation type was properly initialized.     KAC
**********************************************************************/
{
  extern option_struct options;
#if LINK_DEBUG
  extern debug_struct debug;
#endif
#if QUICK_FS
  extern double temps[];
#endif

  char     tmpstr[MAXSTRING];
  char     ErrStr[MAXSTRING];
  char     FIRST_VEG;
  int      i, j, ii, veg, index;
  int      nidx, lidx;
  int      tmpint;
  int      dry;
  int      band;
  int      zindex;
#if SPATIAL_FROST
  int      frost_area;
#endif
  double   Cv;
  double   sum, Lsum, Zsum, dp, Ltotal;
  double   tmpdp, tmpadj;
  double   Tair;
  double  *kappa, *Cs, *M;
  double   moist[MAX_VEG][MAX_BANDS][MAX_LAYERS];
/*   double   ice[MAX_VEG][MAX_BANDS][MAX_LAYERS]; */
  double   unfrozen, frozen;
  double **layer_ice;
  double **layer_tmp;
  double  *EMPTY;
#if QUICK_FS
  double   Aufwc, Bufwc;
#endif
  char    *EMPTY_C;

  cell_data_struct     ***cell;
  energy_bal_struct     **energy;
#if LAKE_MODEL
  lake_var_struct   *lake_var;
#endif // LAKE_MODEL
  snow_data_struct      **snow;
  veg_var_struct       ***veg_var;

  cell    = prcp->cell;
  energy  = prcp->energy;
#if LAKE_MODEL
  lake_var = &prcp->lake_var;
#endif // LAKE_MODEL
  snow    = prcp->snow;
  veg_var = prcp->veg_var;
  
  // Initialize distributed precipiation
  if(options.DIST_PRCP) 
    Ndist = 2;
  else 
    Ndist = 1;

  // Initialize soil depths
  dp = soil_con->dp;
  Ltotal = 0;
  for ( index = 0; index < options.Nlayer; index++ ) 
    Ltotal += soil_con->depth[index];

  FIRST_VEG = TRUE;

  // increase initial soil surface temperature if air is very cold
  Tair = surf_temp;
  if ( surf_temp < -1. ) surf_temp = -1.;
  
  /********************************************
    Initialize all snow pack variables 
    - some may be reset if state file present
  ********************************************/

  initialize_snow(snow, Nveg, infiles.init_snow, cellnum);

  /********************************************
    Initialize all soil layer variables 
    - some may be reset if state file present
  ********************************************/

  initialize_soil(cell[WET], soil_con, Nveg);

  /********************************************
    Initialize all vegetation variables 
    - some may be reset if state file present
  ********************************************/

  initialize_veg(veg_var[WET], veg_con, global_param);

  /********************************************
    Initialize all lake variables 
  ********************************************/

#if LAKE_MODEL
  if ( options.LAKES ) {
    initialize_lake(lake_var, lake_con, &snow[Nveg+1][0], surf_temp);
  }
#endif // LAKE_MODEL

  /********************************************
    Initialize all spatial frost variables 
  ********************************************/

#if SPATIAL_FROST
  for ( frost_area = 0; frost_area < FROST_SUBAREAS; frost_area++ ) {
    if ( FROST_SUBAREAS == 1 ) soil_con->frost_fract[frost_area] = 1.;
    else if (FROST_SUBAREAS == 2 ) soil_con->frost_fract[frost_area] = 0.5;
    else {
      soil_con->frost_fract[frost_area] = 1. / (FROST_SUBAREAS - 1);
      if ( frost_area == 0 || frost_area == FROST_SUBAREAS-1 ) 
	soil_con->frost_fract[frost_area] /= 2.;
    }
  }
#endif // SPATIAL_FROST

  /********************************************************
    Compute grid cell fractions for all subareas used in 
    spatial distribution of soil frost routines.
  ********************************************************/

#if QUICK_FS
  if(options.FROZEN_SOIL) {

    /***********************************************************
      Prepare table of maximum unfrozen water content values
      - This linearizes the equation for maximum unfrozen water
        content, reducing computation time for the frozen soil
        model.
    ***********************************************************/

    for(lidx=0;lidx<options.Nlayer;lidx++) { 
      for(ii=0;ii<QUICK_FS_TEMPS;ii++) {
	Aufwc = maximum_unfrozen_water(temps[ii], 1.0, 
				       soil_con->bubble[lidx], 
				       soil_con->expt[lidx]);
	Bufwc = maximum_unfrozen_water(temps[ii+1], 1.0, 
				       soil_con->bubble[lidx], 
				       soil_con->expt[lidx]);
	soil_con->ufwc_table_layer[lidx][ii][0] 
	  = linear_interp(0., temps[ii], temps[ii+1], Aufwc, Bufwc);
	soil_con->ufwc_table_layer[lidx][ii][1] 
	  = (Bufwc - Aufwc) / (temps[ii+1] - temps[ii]);
      }
    }
  }  
#endif // QUICK_FS

  /************************************************************************
    CASE 1: Not using quick ground heat flux, and initial conditions files 
    provided
  ************************************************************************/

  if(options.INIT_STATE) {

    read_initial_model_state(infiles.statefile, prcp, global_param,  
			     Nveg, options.SNOW_BAND, cellnum, soil_con);

    for ( veg = 0 ; veg <= Nveg ; veg++ ) {
      // Initialize soil for existing vegetation types
      if ( veg < Nveg ) Cv = veg_con[veg].Cv;
      else Cv = (1.0 - veg_con[0].Cv_sum);

      if ( Cv > 0 ) {
	for( band = 0; band < options.SNOW_BAND; band++ ) {
	  for( lidx = 0; lidx < options.Nlayer; lidx++ ) {
	    moist[veg][band][lidx] = cell[0][veg][band].layer[lidx].moist;
	  }
	}
      }
    }
  }
  
  /************************************************************************
    CASE 2: Initialize soil if using quick heat flux, and no initial
    soil properties file given
  ************************************************************************/
    
  else if(options.QUICK_FLUX) {

    for ( veg = 0 ; veg <= Nveg ; veg++ ) {
      // Initialize soil for existing vegetation types
      if ( veg < Nveg ) Cv = veg_con[veg].Cv;
      else Cv = (1.0 - veg_con[0].Cv_sum);
      
      if ( Cv > 0 ) {
	for( band = 0; band < options.SNOW_BAND; band++ ) {

	  /* Initialize soil node temperatures and thicknesses */
	  
	  soil_con->dz_node[0]   = soil_con->depth[0];
	  soil_con->dz_node[1]   = soil_con->depth[0];
	  soil_con->dz_node[2]   = 2. * (dp - 1.5 * soil_con->depth[0]);
	  energy[veg][band].T[0] = surf_temp;
	  energy[veg][band].T[1] = surf_temp;
	  energy[veg][band].T[2] = soil_con->avg_temp;
	  
	  for ( lidx = 0; lidx < options.Nlayer; lidx++ ) 
	    moist[veg][band][lidx] = soil_con->init_moist[lidx];
	  
	}
      }
    }
  }

  /*****************************************************************
    CASE 3: Initialize Energy Balance Variables if not using quick
    ground heat flux, and no Initial Condition File Given 
  *****************************************************************/
  else if(!options.QUICK_FLUX) {
    for ( veg = 0 ; veg <= Nveg ; veg++ ) {
      // Initialize soil for existing vegetation types
      if ( veg < Nveg ) Cv = veg_con[veg].Cv;
      else Cv = (1.0 - veg_con[0].Cv_sum);
      
      if ( Cv > 0 ) {
	for( band = 0; band < options.SNOW_BAND; band++ ) {
	  
	  /* Initialize soil node temperatures and thicknesses 
	     Nodes set at surface, the depth of the first layer,
	     twice the depth of the first layer, and at the
	     damping depth.  Extra nodes are placed equal distance
	     between the damping depth and twice the depth of the
	     first layer. */
	  
	  energy[veg][band].T[0] = surf_temp;
	  soil_con->dz_node[0] = soil_con->depth[0];
	  soil_con->dz_node[1] = soil_con->depth[0];
	  soil_con->dz_node[2] = soil_con->depth[0];
	  energy[veg][band].T[Nnodes-1] = soil_con->avg_temp;
	  energy[veg][band].T[1] = exp_interp(soil_con->depth[0], 0., dp, 
					      surf_temp, soil_con->avg_temp);
	  energy[veg][band].T[2] = exp_interp(2. * soil_con->depth[0], 0., dp, 
					      surf_temp, soil_con->avg_temp);
	  
	  Zsum   = 2. * soil_con[0].depth[0];
	  tmpdp  = dp - soil_con[0].depth[0] * 2.5;
	  tmpadj = 3.5;
	  for ( index = 3; index < Nnodes-1; index++ ) {
	    if ( veg == 0 && band == 0 )
	      soil_con->dz_node[index] = tmpdp/(((double)Nnodes-tmpadj));
	    Zsum += (soil_con->dz_node[index]
		     +soil_con->dz_node[index-1])/2.;
	    energy[veg][band].T[index] = exp_interp(Zsum,0.,soil_con[0].dp,
						    surf_temp,
						    soil_con[0].avg_temp);
	  }
	  if ( veg == 0 && band == 0) {
	    soil_con->dz_node[Nnodes-1] = (dp - Zsum 
					   - soil_con->dz_node[Nnodes-2] 
					   / 2. ) * 2.;
	    Zsum += (soil_con->dz_node[Nnodes-2]
		     +soil_con->dz_node[Nnodes-1])/2.;
	    if((int)(Zsum*1000+0.5) != (int)(dp*1000+0.5)) {
	      sprintf(ErrStr,"Sum of thermal node thicknesses (%f) in initialize_model_state do not equal dp (%f), check initialization procedure",Zsum,dp);
	      nrerror(ErrStr);
	    }
	  }

	  for ( lidx = 0; lidx < options.Nlayer; lidx++ )
	    moist[veg][band][lidx] = soil_con->init_moist[lidx];
	}
      }
    }
  }

  /*********************************
    CASE 4: Unknown option
  *********************************/
  else {
    for ( veg = 0 ; veg <= Nveg ; veg++ ) {
      // Initialize soil for existing vegetation types
      if ( veg < Nveg ) Cv = veg_con[veg].Cv;
      else Cv = (1.0 - veg_con[0].Cv_sum);

      if ( Cv > 0 ) {
	for( band = 0; band < options.SNOW_BAND; band++ ) {
	  // Initialize soil for existing snow elevation bands
	  if ( soil_con->AreaFract[band] > 0. ) {
	  
	    for ( index = 0; index < options.Nlayer; index++ ) {
	      soil_con->dz_node[index] = 1.;
	    }
	  }
	}
      }
    }
  }

  /******************************************
    Initialize soil thermal node properties 
  ******************************************/

  if ( options.GRND_FLUX ) {

    for ( veg = 0 ; veg <= Nveg ; veg++) {
      // Initialize soil for existing vegetation types
      if ( veg < Nveg ) Cv = veg_con[veg].Cv;
      else Cv = (1.0 - veg_con[0].Cv_sum);

      if ( Cv > 0 ) {
	for( band = 0; band < options.SNOW_BAND; band++ ) {
	  // Initialize soil for existing snow elevation bands
	  if ( soil_con->AreaFract[band] > 0. ) {
	  
	    /** Set soil properties for all soil nodes **/
	    if(FIRST_VEG) {
	      FIRST_VEG = FALSE;
	      set_node_parameters(soil_con->dz_node, soil_con->max_moist_node,
				  soil_con->expt_node, soil_con->bubble_node,
				  soil_con->alpha, soil_con->beta,
				  soil_con->gamma, soil_con->depth,
				  soil_con->max_moist, soil_con->expt, 
				  soil_con->bubble, soil_con->quartz, 
				  soil_con->layer_node_fract,
#if QUICK_FS
				  soil_con->ufwc_table_node,
#endif // QUICK_FS
				  Nnodes, options.Nlayer, soil_con->FS_ACTIVE);
	  
	      sum = soil_con->dz_node[0]/2. + soil_con->dz_node[Nnodes-1]/2.;
	      for ( nidx = 1; nidx < Nnodes-1; nidx++ ) 
		sum += soil_con->dz_node[nidx];
	    }
	
	    /* set soil moisture properties for all soil thermal nodes */
	    distribute_node_moisture_properties(energy[veg][band].moist,
						energy[veg][band].ice,
						energy[veg][band].kappa_node,
						energy[veg][band].Cs_node,
						soil_con->dz_node,
						energy[veg][band].T,
						soil_con->max_moist_node,
#if QUICK_FS
						soil_con->ufwc_table_node,
#else
						soil_con->expt_node,
						soil_con->bubble_node,
#endif // QUICK_FS
						moist[veg][band], 
						soil_con->depth,
						soil_con->soil_density,
						soil_con->bulk_density,
						soil_con->quartz,
						Nnodes, options.Nlayer,
						soil_con->FS_ACTIVE);
	
	    /* initialize layer moistures and ice contents */
	    for ( dry = 0; dry < Ndist; dry++ ) {
	      for ( lidx = 0; lidx < options.Nlayer; lidx++ ) {
		cell[dry][veg][band].layer[lidx].moist 
		  = moist[veg][band][lidx];
	      }
	      estimate_layer_ice_content(cell[dry][veg][band].layer,
					 soil_con->dz_node,
					 energy[veg][band].T,
					 soil_con->max_moist_node,
#if QUICK_FS
					 soil_con->ufwc_table_node,
#else
					 soil_con->expt_node,
					 soil_con->bubble_node,
#endif // QUICK_FS
					 soil_con->depth,
					 soil_con->max_moist,
#if QUICK_FS
					 soil_con->ufwc_table_layer,
#else
					 soil_con->expt,
					 soil_con->bubble,
#endif // QUICK_FS
#if SPATIAL_FROST
					 soil_con->frost_fract, 
					 soil_con->frost_slope, 
#endif // SPATIAL_FROST
					 soil_con->bulk_density,
					 soil_con->soil_density,
					 soil_con->quartz, 
					 soil_con->layer_node_fract,
					 Nnodes, options.Nlayer, 
					 soil_con->FS_ACTIVE);
	  
	    }
	
	    /* Find freezing and thawing front depths */
	    if(!options.QUICK_FLUX && soil_con->FS_ACTIVE) 
	      find_0_degree_fronts(&energy[veg][band], soil_con->dz_node,
				   energy[veg][band].T, Nnodes);
	  }
	}
      }
    }	
  }

  // initialize energy balance terms before iterations begin
  for ( veg = 0 ; veg <= Nveg ; veg++) {
    for ( band = 0; band < options.SNOW_BAND; band++ ) {
	energy[veg][band].LongUnderOut = 0.;
	energy[veg][band].Tfoliage     = Tair;
    }
  }


}
