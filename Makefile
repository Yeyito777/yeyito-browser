.PHONY: install install-dirty

install:
	./scripts/install.sh $(ARGS)

install-dirty:
	./scripts/install.sh --dirty $(ARGS)
