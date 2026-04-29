.PHONY: install install-dirty \
	webengine-dev webengine-dev-build webengine-dev-install webengine-dev-launcher \
	webengine-dev-status webengine-cache-status webengine-cache-enable

install:
	./scripts/install.sh $(ARGS)

install-dirty:
	./scripts/install.sh --dirty $(ARGS)

# Fast Chromium/QtWebEngine inner-loop targets. Use these after one full
# install-dirty has configured and populated build/install.
webengine-dev:
	./scripts/webengine-dev.sh build-install

webengine-dev-build:
	./scripts/webengine-dev.sh build

webengine-dev-install:
	./scripts/webengine-dev.sh install

webengine-dev-launcher:
	./scripts/webengine-dev.sh launcher

webengine-dev-status:
	./scripts/webengine-dev.sh status

webengine-cache-status:
	./scripts/webengine-dev.sh cache-status

webengine-cache-enable:
	./scripts/webengine-dev.sh enable-cache
