fluxpy_PYTHON=\
	      __init__.py\
	      kvs.py\
	      wrapper.py\
	      rpc.py\
	      message.py\
	      constants.py\
	      job.py \
	      mrpc.py \
	      util.py \
	      future.py

if HAVE_FLUX_SECURITY
fluxpy_PYTHON += security.py
endif

if ENABLE_PYLINT
#TODO: there must be a better way to do this
# scan flux bindings with pylint, currently fails on any exit but 0
check-local:
	if [ -x "$$( which pylint )" ] ; then  pylint --rcfile=$(top_srcdir)/src/bindings/python/.pylintrc $(top_srcdir)/src/bindings/python/flux; fi
endif

clean-local:
	-rm -f *.pyc *.pyo
	-rm -rf __pycache__

SUBDIRS = core

install-data-hook:
	$(AM_V_at)echo Linking python modules in non-standard location... && \
	  $(INSTALL) -d -m 0755 "$(DESTDIR)$(fluxpylinkdir)" && \
	  target=$(fluxpydir) && \
	  f=$${target##*/} && \
	  cd "$(DESTDIR)$(fluxpylinkdir)" && \
	  rm -f $$f && \
	  $(LN_S) $$target .

uninstall-local:
	$(AM_V_at)target=$(fluxpydir) && f=$${target##*/} && \
	  echo "Removing $(fluxpylinkdir)/$$f" && \
	  rm -rf $(fluxpylinkdir)/$$f
