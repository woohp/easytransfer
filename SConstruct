env = Environment(
    CXX = 'g++',
    CXXFLAGS = ['-Wall', '-pedantic', '-g'],
    LIBS = ['boost_filesystem-mt', 'boost_system-mt', 'boost_program_options-mt', 'miniupnpc', 'dl', 'archive'],
    CPPPATH = '.'
)

easytransfer = env.Program('easytransfer', ['easytransfer.cpp', 'mongoose.c'])

# for installation
env.Alias('install', '/usr/local/bin')
env.Install('/usr/local/bin', easytransfer)

# for uninstallation
# env.Command("uninstall-all", None, Delete(FindInstalledFiles()))
# env.Alias('uninstall', 'uninstall-all')
