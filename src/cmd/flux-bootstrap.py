from __future__ import print_function

import os
import sys
import shutil
import inspect
import logging
import argparse
import tempfile
import itertools
from enum import Enum
from subprocess import Popen, PIPE

from flux.job import JobSpec
from flux import util


class ResourceManager(object):
    """Base class for describing a Resource Manager"""

    def __init__(self, jobspec):
        """Initializes with a flux jobspec
        """
        self.jobspec = JobSpec(jobspec)

    def args(self):
        """Return the set of args to use, translating if available

        Translates a Flux jobspec into the set of args required by the
        underlying RM to get an allocation.

        Calls every defined map method with the loaded jobspec
        """

        args = (
            m()
            for name, m in inspect.getmembers(self, inspect.ismethod)
            if name.startswith("map_")
        )
        return [arg for arg in args if arg is not None]

    def submit(self, dry_run=False):
        raise NotImplementedError("must implement submit")


def seconds_to_walltime(seconds):
    hours, remainder = divmod(seconds, 3600)
    minutes, seconds = divmod(remainder, 60)
    return "{:02d}:{:02d}:{:02d}".format(int(hours), int(minutes), int(seconds))


class Slurm(ResourceManager):

    default_shell = "/bin/sh"

    def job_script(self, args, jobspec_str):
        """Takes a set of slurm args and outputs a jobscript
        """
        try:
            shell = self.jobspec.attributes["user"]["shell"]
        except KeyError:
            shell = self.default_shell

        script = "#!{shell}\n".format(shell=shell)
        script += (
            "\n".join("#SBATCH " + " ".join(arg_tuple) for arg_tuple in args) + "\n"
        )
        script += "echo '{}' > /tmp/test.yaml\n".format(jobspec_str)
        script += "srun -N ${SLURM_JOB_NUM_NODES} -n ${SLURM_NTASKS} --mpibind=off --cpu-bind=none flux start bash -c 'flux job attach $(flux job submit /tmp/test.yaml)'\n"

        return script

    def interactive_command(self, args, jobspec_str):
        command = ["srun", "--mpibind=off", "--cpu-bind=none"]
        command.extend(list(itertools.chain.from_iterable(args)))
        command.extend(
            [
                "flux",
                "start",
                "bash",
                "-c",
                "flux job attach $(flux job submit <(echo '{}')".format(jobspec_str),
            ]
        )
        return command

    def map_mincpus_per_node(self):
        return [
            "--mincpus={}".format(
                self.jobspec.total_num_cores / self.jobspec.total_num_nodes
            )
        ]

    # def map_resource_cores(self):
    #     '''This does not request the exact number of cores per task that the user job
    #     requires. This argument, in concert with `-c` ensures that we get enough
    #     total cores for the user job.
    #     '''
    #     return "-c {:d}".format(

    # def map_task_tasks(self):
    #     '''This does not request the actual number of tasks that the user job
    #     requires. This argument, in concert with `-c` ensures that we get enough
    #     total cores for the user job.
    #     '''
    #     return "-n {:d}".format(self.jobspec.total_num_nodes)

    def map_resource_nodes(self):
        nnodes = self.jobspec.total_num_nodes
        if nnodes > 0:
            return ["-N", "{:d}".format(nnodes)]
        return None

    def map_attribute_system(self):
        duration = self.jobspec.duration
        if duration is None:
            return None
        else:
            # the duration in JobSpec is stored as seconds,
            # convert back to Slurm-style D-HH:MM:SS
            walltime = seconds_to_walltime(duration)
            return ["-t", "{}".format(walltime)]

    def submit(self, dry_run=False):
        # command = self.interactive_command(self.args(), self.jobspec.dumps())
        # if dry_run:
        #    print(command)
        #    return
        #
        # self.proc = Popen(command, stdin=None, stdout=PIPE, stderr=PIPE)
        # out, err = self.proc.communicate()
        # print(err, file=sys.stderr)
        # print(out)

        script = self.job_script(self.args(), self.jobspec.dumps())
        if dry_run:
            print(script)
            return
        self.proc = Popen(["sbatch"], stdin=PIPE, stdout=PIPE, stderr=PIPE)
        out, err = self.proc.communicate(input=script)
        if len(err) > 0:
            print(err, file=sys.stderr)
        if len(out) > 0:
            print(out)


class LSF(ResourceManager):
    pass


class Cobalt(ResourceManager):
    pass


class Flux(ResourceManager):
    pass


ResourceManagers = Enum(
    "ResourceManagers", {rm.__name__: rm for rm in ResourceManager.__subclasses__()}
)


def create_manager(manager_type, jobspec):
    """Create a new resource manager given a manager_type"""
    t = manager_type
    if t in ResourceManagers:
        return manager_type.value(jobspec)
    else:
        raise ValueError("manager type {t} not found".format(t=t))


def detect_manager(types):
    """Detect the resource manager on this host"""

    def which(cmd):
        all_paths = (
            os.path.join(path, cmd) for path in os.environ["PATH"].split(os.pathsep)
        )

        return any(
            os.access(path, os.X_OK) and os.path.isfile(path) for path in all_paths
        )

    if os.getenv("FLUX_URI") is not None:
        return types.Flux
    if which("bsub"):
        return types.LSF
    elif which("salloc"):
        return types.Slurm
    elif which("cqsub"):
        return types.Cobalt
    else:
        raise OSError("unable to find a resource manager on this system")


def bootstrap(jobspec, dry_run=False):
    """Detect a resource manager and start flux with a given jobspec"""

    try:
        t = detect_manager(ResourceManagers)
        resource_manager = create_manager(t, jobspec)
        resource_manager.submit(dry_run=dry_run)
    except OSError as e:
        sys.exit(e)
    sys.exit(0)


logger = logging.getLogger("flux-bootstrap")


@util.CLIMain(logger)
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "-n",
        "--dry-run",
        action="store_true",
        help="""Print the submission command or script""",
    )
    parser.add_argument("jobspec")
    cli_args = parser.parse_args()
    if cli_args.jobspec == "-":
        logger.warn("Reading jobspec from stdin")
        jobspec = sys.stdin.readline()
    else:
        with open(cli_args.jobspec, "r") as infile:
            jobspec = infile.readline()
    bootstrap(jobspec, dry_run=cli_args.dry_run)


if __name__ == "__main__":
    main()
