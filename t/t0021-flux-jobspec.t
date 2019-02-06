#!/bin/sh

test_description='Test the flux-jobspec command'

. `dirname $0`/sharness.sh

JOBSPEC=${SHARNESS_TEST_SRCDIR}/jobspec
VALIDATE=${JOBSPEC}/validate.py
SCHEMA=${FLUX_SOURCE_DIR}/src/modules/job-ingest/schemas/jobspec.jsonschema
MINI_SCHEMA=${JOBSPEC}/minimal-schema.json

validate_emission() {
    flux jobspec $@ | ${VALIDATE} --schema ${SCHEMA}
}

validate_minimal_emission() {
    flux jobspec $@ | ${VALIDATE} --schema ${MINI_SCHEMA}
}

test_expect_success 'flux-jobspec srun with no args emits valid canonical jobspec' '
    validate_emission srun sleep 1
'

test_expect_success 'flux-jobspec srun with no args emits minimal jobspec' '
    validate_minimal_emission srun sleep 1
'

test_expect_success 'flux-jobspec srun with just num_tasks emits valid canonical jobspec' '
    validate_emission srun -n4 sleep 1
'

test_expect_success 'flux-jobspec srun with just num_tasks emits minimal jobspec' '
    validate_minimal_emission srun -n4 sleep 1
'

test_expect_success 'flux-jobspec srun with just cores_per_task emits valid canonical jobspec' '
    validate_emission srun -c4 sleep 1
'

test_expect_success 'flux-jobspec srun with just cores_per_task emits minimal jobspec' '
    validate_minimal_emission srun -c4 sleep 1
'

test_expect_success 'flux-jobspec srun without num_nodes emits valid canonical jobspec' '
    validate_emission srun -n4 -c1 sleep 1
'

test_expect_success 'flux-jobspec srun without num_nodes emits minimal jobspec' '
    validate_minimal_emission srun -n4 -c1 sleep 1
'

test_expect_success 'flux-jobspec srun with all args emits valid canonical jobspec' '
    validate_emission srun -N4 -n4 -c1 sleep 1
'

test_expect_success 'flux-jobspec srun with all args emits minimal jobspec' '
    validate_minimal_emission srun -N4 -n4 -c1 sleep 1
'
test_done
