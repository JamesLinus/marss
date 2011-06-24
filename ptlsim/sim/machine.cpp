
#include <machine.h>
#include <ptlsim.h>
#include <config.h>

#include <basecore.h>
#include <stats.h>
#include <statsBuilder.h>
#include <memoryHierarchy.h>

#define INSIDE_DEFCORE
#include <defcore.h>

#include <atomcore.h>

#include <cstdarg>
#include <sched.h>
#include <signal.h>

using namespace Core;
using namespace Memory;

/* Machine Generator Functions */
MachineBuilder machineBuilder("_default_", NULL);

BaseMachine::BaseMachine(const char *name)
{
    machine_name = name;
    addmachine(machine_name, this);

    stringbuf stats_name;
    stats_name << "base_machine";
    update_name(stats_name.buf);

    context_used = 0;
    coreid_counter = 0;
}

BaseMachine::~BaseMachine()
{
    foreach(i, pthreads.count()) {
        pthread_kill(*pthreads[i], 9);
    }

    removemachine(machine_name, this);
}

void BaseMachine::reset()
{
    context_used = 0;
    context_counter = 0;
    coreid_counter = 0;

    foreach(i, cores.count()) {
        BaseCore* core = cores[i];
        delete core;
    }

    cores.clear();

    if(memoryHierarchyPtr) {
        delete memoryHierarchyPtr;
        memoryHierarchyPtr = NULL;
    }
}

W8 BaseMachine::get_num_cores()
{
    return cores.count();
}

bool BaseMachine::init(PTLsimConfig& config)
{
    int context_idx = 0;

    config.cache_config_type = "auto";

    // At the end create a memory hierarchy
    memoryHierarchyPtr = new MemoryHierarchy(*this);

    if(config.machine_config == "") {
        ptl_logfile << "[ERROR] Please provide Machine name in config using -machine\n" << flush;
        cerr << "[ERROR] Please provide Machine name in config using -machine\n" << flush;
        assert(0);
    }

    machineBuilder.setup_machine(*this, config.machine_config.buf);

    foreach(i, cores.count()) {
        cores[i]->update_memory_hierarchy_ptr();
    }

    setup_threads();

    return 1;
}

void BaseMachine::setup_threads()
{
    int num_cores;
    int num_threads;

    if(!config.threaded_simulation)
        return;

    num_cores = cores.count();

    if(num_cores <= config.cores_per_pthread ||
            logable(1)) {
        config.threaded_simulation = 0;
        ptl_logfile << "Disabled Threaded simulation because ",
                    "cores_per_pthread < number of simulated cores.\n";
        return;
    }

    /* Count number of thread to create */
    num_threads = ceil(num_cores / config.cores_per_pthread);
    cerr << "Num threads " << num_threads << endl;
    ptl_logfile << "Num threads " << num_threads << endl;

    /* Setup barriers and mutex */
    exit_mutex = new pthread_mutex_t();
    pthread_mutex_init(exit_mutex, NULL);

    access_mutex = new pthread_mutex_t();
    pthread_mutex_init(access_mutex, NULL);

    runcycle_barrier = new pthread_barrier_t();
    pthread_barrier_init(runcycle_barrier, NULL, num_threads + 1);

    exit_process_barrier = new pthread_barrier_t();
    pthread_barrier_init(exit_process_barrier, NULL, num_threads + 1);

    foreach(i, num_threads) {
        int rc;
        pthread_attr_t attr;
        cpu_set_t cpu_set;

        if(pthread_attr_init(&attr)) {
            ptl_logfile << "[ERROR] [PTHREAD] Can't initialize attr\n";
            cerr << "[ERROR] [PTHREAD] Can't initialize attr\n";
            assert(0);
        }

        CPU_ZERO(&cpu_set);
        CPU_SET(i, &cpu_set);

        if(pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpu_set)) {
            ptl_logfile << "[WARN] [PTHREAD] Can't set cpu affinity\n";
            cerr << "[WARN] [PTHREAD] Can't set cpu affinity\n";
        }

        pthread_t *th = new pthread_t();
        PthreadArg *th_arg = new PthreadArg(this, i * config.cores_per_pthread);

        if((rc = pthread_create(th, &attr, &BaseMachine::start_thread,
                        th_arg))) {
            ptl_logfile << "[ERROR] [PTHREAD] Can't create a pthread\n";
            cerr << "[ERROR] [PTHREAD] Can't create a pthread\n";
            assert(0);
        }

        pthreads.push(th);

        pthread_attr_destroy(&attr);
    }
}

void *BaseMachine::start_thread(void *arg)
{
    PthreadArg *th_arg = (PthreadArg*)arg;
    reinterpret_cast<BaseMachine*>(th_arg->obj)->run_cores_thread(
            th_arg->start_id);
}

int BaseMachine::run(PTLsimConfig& config)
{
    if(logable(1))
        ptl_logfile << "Starting base core toplevel loop", endl, flush;

    // All VCPUs are running:
    stopped = 0;
    if unlikely (config.start_log_at_iteration &&
            iterations >= config.start_log_at_iteration &&
            !config.log_user_only) {

        if unlikely (!logenable)
            ptl_logfile << "Start logging at level ",
                        config.loglevel, " in cycle ",
                        iterations, endl, flush;

        logenable = 1;
    }

    // reset all cores for fresh start:
    foreach (cur_core, cores.count()){
        BaseCore& core =* cores[cur_core];
        if(first_run) {
            cores[cur_core]->reset();
        }
        cores[cur_core]->check_ctx_changes();
    }
    first_run = 0;

    // Run each core
    bool exiting = false;

    if(config.threaded_simulation) {
        return run_threaded();
    }

    for (;;) {
        if unlikely ((!logenable) &&
                iterations >= config.start_log_at_iteration &&
                !config.log_user_only) {
            ptl_logfile << "Start logging at level ", config.loglevel,
                        " in cycle ", iterations, endl, flush;
            logenable = 1;
        }

        if(sim_cycle % 1000 == 0)
            update_progress();

        if unlikely(sim_cycle == 0 && time_stats_file)
            StatsBuilder::get().dump_header(*time_stats_file);

        // TODO: make this a config param?
        if unlikely(sim_cycle % 10000 == 0 && time_stats_file)
            StatsBuilder::get().dump_periodic(*time_stats_file, sim_cycle);


        // limit the ptl_logfile size
        if unlikely (ptl_logfile.is_open() &&
                (ptl_logfile.tellp() > config.log_file_size))
            backup_and_reopen_logfile();

        memoryHierarchyPtr->clock();

        foreach (cur_core, cores.count()){
            BaseCore& core =* cores[cur_core];

            if(logable(4))
                ptl_logfile << "cur_core: ", cur_core, " running [core ",
                            core.get_coreid(), "]", endl;
            exiting |= core.runcycle();
        }

        // Collect total number of instructions committed
        total_user_insns_committed = 0;
        foreach(i, cores.count()) {
            total_user_insns_committed += cores[i]->get_insns_committed();
        }

        global_stats.summary.cycles++;
        sim_cycle++;
        iterations++;

        if unlikely (config.wait_all_finished ||
                config.stop_at_user_insns <= total_user_insns_committed){
            ptl_logfile << "Stopping simulation loop at specified limits (", iterations, " iterations, ", total_user_insns_committed, " commits)", endl;
            exiting = 1;
            break;
        }
        if unlikely (exiting) {
            if unlikely(ret_qemu_env == NULL)
                ret_qemu_env = &contextof(0);
            break;
        }
    }

    if(logable(1))
        ptl_logfile << "Exiting out-of-order core at ", total_user_insns_committed, " commits, ", total_uops_committed, " uops and ", iterations, " iterations (cycles)", endl;

    config.dump_state_now = 0;

    return exiting;
}

bool BaseMachine::run_threaded()
{
    bool exiting = false;

    for(;;) {

        if(config.start_log_at_iteration &&
                iterations >= config.start_log_at_iteration) {
            config.threaded_simulation = 0;
            return false;
        }

        if(sim_cycle % 10000 == 0) {
            update_progress();
        }

        if unlikely(sim_cycle == 0 && time_stats_file)
            StatsBuilder::get().dump_header(*time_stats_file);

        // TODO: make this a config param?
        if unlikely(sim_cycle % 10000 == 0 && time_stats_file)
            StatsBuilder::get().dump_periodic(*time_stats_file, sim_cycle);

        // limit the ptl_logfile size
        if unlikely (ptl_logfile.is_open() &&
                (ptl_logfile.tellp() > config.log_file_size))
            backup_and_reopen_logfile();

        memoryHierarchyPtr->clock();

        // Now send signal to all threads to run one cycle
        pthread_barrier_wait(runcycle_barrier);

        // Wait for all threads to simulate one cycle
        pthread_barrier_wait(exit_process_barrier);

        // Check 'exit_requested' and exit if requested
        pthread_mutex_lock(exit_mutex);
        exiting = exit_requested;
        exit_requested = 0;
        pthread_mutex_unlock(exit_mutex);

        // Collect total number of instructions committed
        total_user_insns_committed = 0;
        foreach(i, cores.count()) {
            total_user_insns_committed += cores[i]->get_insns_committed();
        }

        global_stats.summary.cycles++;
        sim_cycle++;
        iterations++;

        if unlikely (config.wait_all_finished ||
                config.stop_at_user_insns <= total_user_insns_committed) {
            ptl_logfile << "Stopping simulation loop at specified limits (",
                        iterations, " iterations, ", total_user_insns_committed,
                        " commits)", endl;
            exiting = 1;
            break;
        }

        if unlikely (exiting) {
            if unlikely(ret_qemu_env == NULL)
                ret_qemu_env = &contextof(0);
            break;
        }
    }

    if(logable(1))
        ptl_logfile << "Exiting machine::run at ", total_user_insns_committed, " commits, ", total_uops_committed, " uops and ", iterations, " iterations (cycles)", endl;

    config.dump_state_now = 0;

    return exiting;
}

void BaseMachine::run_cores_thread(int start_id)
{
    int start_coreid = start_id;
    int end_coreid = min(int(start_coreid + config.cores_per_pthread),
            cores.count());
    dynarray<BaseCore*> mycores;

    cerr << "Start ", start_coreid, " End ", end_coreid, endl;

    pthread_mutex_lock(access_mutex);
    for(int i=start_coreid; i < end_coreid; i++) {
        mycores.push(cores[i]);
    }
    pthread_mutex_unlock(access_mutex);

    for(;;) {
        bool exiting = 0;

        /* Wait for main thread before simulate one cycle */
        pthread_barrier_wait(runcycle_barrier);

        // if(start_coreid == 0) {
            // config.loglevel = 10;
            // logenable = 1;
        // }

        /* Now run one simulation cycle for each assigned core */
        // for(int i=start_coreid; i < end_coreid; i++) {
        foreach(i, mycores.count()) {
            BaseCore& core =* mycores[i];
            exiting |= core.runcycle();
        }

        /* Check exit request and set global exit request if true */
        if(exiting) {
            pthread_mutex_lock(exit_mutex);
            exit_requested = exiting;
            pthread_mutex_unlock(exit_mutex);
        }

        /* Wait for main thread to handle exit requests */
        pthread_barrier_wait(exit_process_barrier);
    }
}

void BaseMachine::flush_tlb(Context& ctx)
{
    foreach(i, cores.count()) {
        BaseCore* core = cores[i];
        core->flush_tlb(ctx);
    }
}

void BaseMachine::flush_tlb_virt(Context& ctx, Waddr virtaddr)
{
    foreach(i, cores.count()) {
        BaseCore* core = cores[i];
        core->flush_tlb_virt(ctx, virtaddr);
    }
}

void BaseMachine::dump_state(ostream& os)
{
    foreach(i, cores.count()) {
        cores[i]->dump_state(os);
    }

    os << " MemoryHierarchy:", endl;
    memoryHierarchyPtr->dump_info(os);
}

void BaseMachine::flush_all_pipelines()
{
    // TODO
}

void BaseMachine::update_stats(PTLsimStats* stats)
{
    // First add user and kernel stats to global stats
    global_stats += user_stats + kernel_stats;

    *n_global_stats += *n_user_stats;
    *n_global_stats += *n_kernel_stats;

    foreach(i, cores.count()) {
        cores[i]->update_stats(stats);
    }
}

Context& BaseMachine::get_next_context()
{
    assert(context_counter < NUM_SIM_CORES);
    assert(context_counter < MAX_CONTEXTS);
    context_used[context_counter] = 1;
    return contextof(context_counter++);
}

W8 BaseMachine::get_next_coreid()
{
    assert((coreid_counter) < MAX_CONTEXTS);
    return coreid_counter++;
}

ConnectionDef* BaseMachine::get_new_connection_def(const char* interconnect,
        const char* name, int id)
{
    ConnectionDef* conn = new ConnectionDef();
    conn->interconnect = interconnect;
    conn->name << name << id;
    connections.push(conn);
    return conn;
}

void BaseMachine::add_new_connection(ConnectionDef* conn,
        const char* cont, int type)
{
    SingleConnection* sg = new SingleConnection();
    sg->controller = cont;
    sg->type = type;

    conn->connections.push(sg);
}

void BaseMachine::setup_interconnects()
{
    foreach(i, connections.count()) {
        ConnectionDef* connDef = connections[i];

        InterconnectBuilder** builder = 
            InterconnectBuilder::interconnectBuilders->get(connDef->interconnect);

        if(!builder) {
            stringbuf err;
            err << "::ERROR::Can't find Interconnect Builder '"
                << connDef->interconnect
                << "'. Please check your config file." << endl;
            ptl_logfile << err;
            cout << err;
            assert(builder);
        }

        Interconnect* interCon = (*builder)->get_new_interconnect(
                *memoryHierarchyPtr,
                connDef->name.buf);
        interconnects.push(interCon);

        foreach(j, connDef->connections.count()) {
            SingleConnection* sg = connDef->connections[j];

            Controller** cont = controller_hash.get(
                    sg->controller);
            assert(cont);

            interCon->register_controller(*cont);
            (*cont)->register_interconnect(interCon, sg->type);
        }
    }
}

void BaseMachine::add_option(const char* name, const char* opt,
        bool value)
{
    BoolOptions** b = bool_options.get(name);
    if(!b) {
        BoolOptions* opts = new BoolOptions();
        bool_options.add(name, opts);
        b = &opts;
    }

    (*b)->add(opt, value);
}

void BaseMachine::add_option(const char* name, const char* opt,
        int value)
{
    IntOptions** b = int_options.get(name);
    if(!b) {
        IntOptions* opts = new IntOptions();
        int_options.add(name, opts);
        b = &opts;
    }

    (*b)->add(opt, value);
}

void BaseMachine::add_option(const char* name, const char* opt,
        const char* value)
{
    StrOptions** b = str_options.get(name);
    if(!b) {
        StrOptions* opts = new StrOptions();
        str_options.add(name, opts);
        b = &opts;
    }

    stringbuf* val = new stringbuf();
    *val << value;
    (*b)->add(opt, val);
}

void BaseMachine::add_option(const char* c_name, int i, const char* opt,
        bool value)
{
    stringbuf core_name;
    core_name << c_name << i;
    add_option(core_name.buf, opt, value);
}

void BaseMachine::add_option(const char* c_name, int i, const char* opt,
        int value)
{
    stringbuf core_name;
    core_name << c_name << i;
    add_option(core_name.buf, opt, value);
}

void BaseMachine::add_option(const char* c_name, int i, const char* opt,
        const char* value)
{
    stringbuf core_name;
    core_name << c_name << i;
    add_option(core_name.buf, opt, value);
}

bool BaseMachine::get_option(const char* name, const char* opt_name,
        bool& value)
{
    BoolOptions** b = bool_options.get(name);
    if(b) {
        bool* bt = (*b)->get(opt_name);
        if(bt) {
            value = *bt;
            return true;
        }
    }

    return false;
}

bool BaseMachine::get_option(const char* name, const char* opt_name,
        int& value)
{
    IntOptions** b = int_options.get(name);
    if(b) {
        int* bt = (*b)->get(opt_name);
        if(bt) {
            value = *bt;
            return true;
        }
    }

    return false;
}

bool BaseMachine::get_option(const char* name, const char* opt_name,
        stringbuf& value)
{
    StrOptions** b = str_options.get(name);
    if(b) {
        stringbuf** bt = (*b)->get(opt_name);
        if(bt) {
            value << **bt;
            return true;
        }
    }

    return false;
}

BaseMachine coremodel("base");

/* Machine Builder */
MachineBuilder::MachineBuilder(const char* name, machine_gen gen)
{
    if(!machineBuilders) {
        machineBuilders = new Hashtable<const char*, machine_gen, 1>();
    }
    machineBuilders->add(name, gen);
}

void MachineBuilder::setup_machine(BaseMachine &machine, const char* name)
{
    machine_gen* gen = machineBuilders->get(name);
    if(!gen) {
        stringbuf err;
        err << "::ERROR::Can't find '" << name
           << "' machine generator." << endl;
        ptl_logfile << err;
        cerr << err;
        assert(gen);
    }

    (*gen)(machine);
}

stringbuf& MachineBuilder::get_all_machine_names(stringbuf& names)
{
    dynarray< KeyValuePair<const char*, machine_gen> > machines;
    machines = machineBuilders->getentries(machines);

    foreach(i, machines.count()) {
        if(machines[i].value)
            names << machines[i].key << ", ";
    }

    return names;
}

Hashtable<const char*, machine_gen, 1> *MachineBuilder::machineBuilders = NULL;

/* CoreBuilder */

CoreBuilder::CoreBuilder(const char* name)
{
    if(!coreBuilders) {
        coreBuilders = new Hashtable<const char*, CoreBuilder*, 1>();
    }
    coreBuilders->add(name, this);
}

Hashtable<const char*, CoreBuilder*, 1> *CoreBuilder::coreBuilders = NULL;

void CoreBuilder::add_new_core(BaseMachine& machine,
        const char* name, const char* core_name)
{
    stringbuf core_name_t;
    ptl_logfile << name;
    core_name_t << name << machine.coreid_counter;
    CoreBuilder** builder = coreBuilders->get(core_name);

    if(!builder) {
        stringbuf err;
        err << "::ERROR::Can't find Core Builder '" << core_name
            << "'. Please check your config file." << endl;
        ptl_logfile << err;
        cout << err;
        assert(builder);
    }

    BaseCore* core = (*builder)->get_new_core(machine, core_name_t.buf);
    machine.cores.push(core);
}

/* Cache Controller Builders */

ControllerBuilder::ControllerBuilder(const char* name)
{
    if(!controllerBuilders) {
        controllerBuilders = new Hashtable<const char*, ControllerBuilder*, 1>();
    }
    controllerBuilders->add(name, this);
}

Hashtable<const char*, ControllerBuilder*, 1>
    *ControllerBuilder::controllerBuilders = NULL;

void ControllerBuilder::add_new_cont(BaseMachine& machine, W8 coreid,
        const char* name, const char* cont_name, W8 type)
{
    stringbuf cont_name_t;
    cont_name_t << name << coreid;
    ControllerBuilder** builder = ControllerBuilder::controllerBuilders->get(cont_name);

    if(!builder) {
        stringbuf err;
        err << "::ERROR::Can't find Controller Builder '" << cont_name
            << "'. Please check your config file." << endl;
        ptl_logfile << err;
        cout << err;
        assert(builder);
    }

    Controller* cont = (*builder)->get_new_controller(coreid, type,
            *machine.memoryHierarchyPtr, cont_name_t.buf);
    machine.controllers.push(cont);
    machine.controller_hash.add(cont_name_t, cont);
}

/* Cache Interconnect Builders */

InterconnectBuilder::InterconnectBuilder(const char* name)
{
    if(!interconnectBuilders) {
        interconnectBuilders = new Hashtable<const char*,
            InterconnectBuilder*, 1>();
    }
    interconnectBuilders->add(name, this);
}

Hashtable<const char*, InterconnectBuilder*, 1>
    *InterconnectBuilder::interconnectBuilders = NULL;

void InterconnectBuilder::create_new_int(BaseMachine& machine, W8 id,
        const char* name, const char* int_name, int count, ...)
{
    va_list ap;
    char* controller_name;
    int conn_type;
    stringbuf int_name_t;

    int_name_t << name << id;
    InterconnectBuilder** builder = 
        InterconnectBuilder::interconnectBuilders->get(int_name);
    assert(builder);

    Interconnect* interCon = (*builder)->get_new_interconnect(
            *machine.memoryHierarchyPtr, int_name_t.buf);
    machine.interconnects.push(interCon);

    va_start(ap, count);
    foreach(i, count*2) {
        controller_name = va_arg(ap, char*); 
        assert(controller_name);

        Controller** cont = machine.controller_hash.get(
                controller_name);
        assert(cont);

        conn_type = va_arg(ap, int);

        interCon->register_controller(*cont);
        (*cont)->register_interconnect(interCon, conn_type);
    }
    va_end(ap);
}
