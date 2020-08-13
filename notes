In order to make it works on nixos:
replace every : /bin/bash in sh files:

```
find . -type f -name "*.sh" -exec sed -i 's:/bin/bash:/bin/env bash:g' {} +
```

Also replace the shebangs in etc/{rc1,rc2,rc3} files with:

```
#!/usr/bin/env -s bash -e
```



As I understand, the any number of broker can be initated.
For instance, doing this command:
(first enter into the nix-shell)

```
./src/cmd/flux start  --size 128 --verbose -D/tmp/flux
```

This will creates 128 flux brokers.
Then the simulation can be executed with:

```
PYTHONPATH="${PYTHONPATH}:./src/bindings/python" ./src/cmd/flux-simulator.py /home/adfaure/sandbox/flux-core/t/simulator/job-traces/10-single-node.csv 128 128 --log-level 10
```

If my understanding is correct, I may have simulated a jobs using 128 nodes with 128 cores each.


The content of the workload

```csv
JobID,NNodes,NCPUS,Timelimit,Submit,Elapsed
1,128,128,00:10:00,2019-01-01T00:00:00,10:00:00
```

The NNodes seems to correspond to the number of broker, while the NCPUS is the number of CPUS per nodes given to the scheduler.
