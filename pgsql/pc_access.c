/***********************************************************************
* pc_access.c
*
*  Accessor functions for points and patches in PgSQL.
*
* Portions Copyright (c) 2012, OpenGeo
*
***********************************************************************/

#include "pc_pgsql.h"      /* Common PgSQL support for our type */
#include "utils/numeric.h"
#include "funcapi.h"

/* Other SQL functions */
Datum pcpoint_get_value(PG_FUNCTION_ARGS);
Datum pcpatch_from_pcpoint_array(PG_FUNCTION_ARGS);

/* Aggregation functions */
Datum pcpoint_agg_final_pcpatch(PG_FUNCTION_ARGS);
Datum pcpoint_agg_final_array(PG_FUNCTION_ARGS);
Datum pcpoint_agg_transfn(PG_FUNCTION_ARGS);
Datum pcpoint_abs_in(PG_FUNCTION_ARGS);
Datum pcpoint_abs_out(PG_FUNCTION_ARGS);

/**
* Read a named dimension from a PCPOINT
* PC_Get(point pcpoint, dimname text) returns Numeric
*/
PG_FUNCTION_INFO_V1(pcpoint_get_value);
Datum pcpoint_get_value(PG_FUNCTION_ARGS)
{
	SERIALIZED_POINT *serpt = PG_GETARG_SERPOINT_P(0);
	text *dim_name = PG_GETARG_TEXT_P(1);
	char *dim_str;
	float8 double_result;

	PCPOINT *pt = pc_point_deserialize(serpt);
	if ( ! pt )
		PG_RETURN_NULL();	

	dim_str = text_to_cstring(dim_name);		
	if ( ! pc_point_get_double_by_name(pt, dim_str, &double_result) )
	{
		pc_point_free(pt);
		elog(ERROR, "dimension \"%s\" does not exist in schema", dim_str);		
	}
	pfree(dim_str);
	pc_point_free(pt);
	PG_RETURN_DATUM(DirectFunctionCall1(float8_numeric, Float8GetDatum(double_result)));
}

PG_FUNCTION_INFO_V1(pcpatch_from_pcpoint_array);
Datum pcpatch_from_pcpoint_array(PG_FUNCTION_ARGS)
{
	ArrayType *array;
	Datum datum = PG_GETARG_DATUM(0);
	int nelems;
	bits8 *bitmap;
	int bitmask;
	size_t offset = 0;
	int i;
	PCPOINTLIST *pl;
	PCPATCH *pa;
	SERIALIZED_PATCH *serpa;
	uint32_t pcid = 0;

	/* Null array, null geometry (should be empty?) */
	if ( (Pointer *)datum == NULL ) PG_RETURN_NULL();

	array = DatumGetArrayTypeP(datum);

	/* How many things in our array? */
	nelems = ArrayGetNItems(ARR_NDIM(array), ARR_DIMS(array));

	/* PgSQL supplies a bitmap of which array entries are null */
	bitmap = ARR_NULLBITMAP(array);

	/* Empty array? Null return */
	if ( nelems == 0 ) 
		PG_RETURN_NULL();
	
	/* Make our holder */
	pl = pc_pointlist_make(nelems);

	offset = 0;
	bitmap = ARR_NULLBITMAP(array);
	bitmask = 1;
	for ( i = 0; i < nelems; i++ )
	{
		/* Only work on non-NULL entries in the array */
		if ( (bitmap && (*bitmap & bitmask)) || !bitmap )
		{
			SERIALIZED_POINT *serpt = (SERIALIZED_POINT *)(ARR_DATA_PTR(array)+offset);
			PCPOINT *pt;
			
			if ( ! pcid ) 
			{
				pcid = serpt->pcid;
			}
			else if ( pcid != serpt->pcid )
			{
				elog(ERROR, "pcpatch_from_pcpoint_array: pcid mismatch (%d != %d)", serpt->pcid, pcid);
			}
			
			pt = pc_point_deserialize(serpt);
			if ( ! pt )
			{
				elog(ERROR, "pcpatch_from_pcpoint_array: point deserialization failed");
			}
			
			pc_pointlist_add_point(pl, pt);

			offset += INTALIGN(VARSIZE(serpt));			
		}

		/* Advance NULL bitmap */
		if (bitmap)
		{
			bitmask <<= 1;
			if (bitmask == 0x100)
			{
				bitmap++;
				bitmask = 1;
			}
		}
	}
	
	if ( pl->npoints == 0 )
		PG_RETURN_NULL();

	pa = pc_patch_from_points(pl);
	pc_pointlist_free(pl);
	serpa = pc_patch_serialize(pa);
	pc_patch_free(pa);
	PG_RETURN_POINTER(serpa);
}

typedef struct
{
	ArrayBuildState *s;
} abs_trans;

PG_FUNCTION_INFO_V1(pcpoint_abs_in);
Datum pcpoint_abs_in(PG_FUNCTION_ARGS)
{
	ereport(ERROR,(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
	               errmsg("function pcpoint_abs_in not implemented")));
	PG_RETURN_POINTER(NULL);
}

PG_FUNCTION_INFO_V1(pcpoint_abs_out);
Datum pcpoint_abs_out(PG_FUNCTION_ARGS)
{
	ereport(ERROR,(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
	               errmsg("function pcpoint_abs_out not implemented")));
	PG_RETURN_POINTER(NULL);
}


PG_FUNCTION_INFO_V1(pcpoint_agg_transfn);
Datum pcpoint_agg_transfn(PG_FUNCTION_ARGS)
{
	Oid arg1_typeid = get_fn_expr_argtype(fcinfo->flinfo, 1);
	MemoryContext aggcontext;
	abs_trans *a;
	ArrayBuildState *state;
	Datum elem;

	if (arg1_typeid == InvalidOid)
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		         errmsg("could not determine input data type")));

	if (fcinfo->context && IsA(fcinfo->context, AggState))
	{
		aggcontext = ((AggState *) fcinfo->context)->aggcontext;
	}
	else if (fcinfo->context && IsA(fcinfo->context, WindowAggState))
	{
		aggcontext = ((WindowAggState *) fcinfo->context)->aggcontext;
	}
	else
	{
		/* cannot be called directly because of dummy-type argument */
		elog(ERROR, "pcpoint_agg_transfn called in non-aggregate context");
		aggcontext = NULL;  /* keep compiler quiet */
	}

	if ( PG_ARGISNULL(0) )
	{
		a = (abs_trans*) palloc(sizeof(abs_trans));
		a->s = NULL;
	}
	else
	{
		a = (abs_trans*) PG_GETARG_POINTER(0);
	}
	state = a->s;
	elem = PG_ARGISNULL(1) ? (Datum) 0 : PG_GETARG_DATUM(1);
	state = accumArrayResult(state,
	                         elem,
	                         PG_ARGISNULL(1),
	                         arg1_typeid,
	                         aggcontext);
	a->s = state;

	PG_RETURN_POINTER(a);
}




static Datum
pcpoint_agg_final(abs_trans *a, MemoryContext mctx, FunctionCallInfo fcinfo)
{
	ArrayBuildState *state;
	int dims[1];
	int lbs[1];
	state = a->s;
	dims[0] = state->nelems;
	lbs[0] = 1;
	return makeMdArrayResult(state, 1, dims, lbs, mctx, false);	
}

PG_FUNCTION_INFO_V1(pcpoint_agg_final_array);
Datum pcpoint_agg_final_array(PG_FUNCTION_ARGS)
{
	abs_trans *a;
	Datum result = 0;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();   /* returns null iff no input values */
	
	a = (abs_trans*) PG_GETARG_POINTER(0);
	
	result = pcpoint_agg_final(a, CurrentMemoryContext, fcinfo);
	PG_RETURN_DATUM(result);
}

PG_FUNCTION_INFO_V1(pcpoint_agg_final_pcpatch);
Datum pcpoint_agg_final_pcpatch(PG_FUNCTION_ARGS)
{
	abs_trans *a;
	Datum result = 0;
	Datum result_final = 0;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();   /* returns null iff no input values */

	a = (abs_trans*) PG_GETARG_POINTER(0);

	result = pcpoint_agg_final(a, CurrentMemoryContext, fcinfo);
	result_final = DirectFunctionCall1(pcpatch_from_pcpoint_array, result);
	PG_RETURN_DATUM(result_final);
}

