INF CranePkg/Driver/CrDALDxe/CrDALDxe.inf
INF CranePkg/Driver/CmdDBCrDxe/CmdDBCrDxe.inf
INF CranePkg/Driver/SmemCrDxe/SmemCrDxe.inf
# Resource, bus, and serial drivers use normal DEPEX dispatch. APRIORI marks
# an image ready without evaluating its DEPEX, so consumers must stay out of
# this list until their provider protocols have been installed.
