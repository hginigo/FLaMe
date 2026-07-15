import networkx as nx
import matplotlib.pyplot as plt
import random
import math
import sys

def seed_vec(reps, seed):
    if seed:
        random.seed(seed)
    return [random.randint(0, 99999) for _ in range(reps)]

def weight_sm_wo(n, k, p, w, seed=34):
    G = nx.watts_strogatz_graph(n, k, p, seed=seed)
    for u, v in G.edges():
        G[u][v]["weight"] = w
    
    return G

def weighted_small_world(n, k, p, w, weight_mode="distance", seed=34):
    """
    n: number of nodes
    k: neighbors per node
    p: rewiring probability
    weight_mode: "random", "distance", or "uniform"
    """

    G = nx.watts_strogatz_graph(n, k, p, seed=seed)
    #random.seed(seed)

    for u, v in G.edges():
        if weight_mode == "random":
            weight = random.uniform(0.1, 1.0)

        elif weight_mode == "distance":
            # circular distance on ring
            d = min(abs(u - v), n - abs(u - v))
            weight = d  # farther = higher weight

        elif weight_mode == "uniform":
            weight = w

        else:
            raise ValueError("Invalid weight_mode")

        G[u][v]["weight"] = weight

    return G


def draw_graph(G):
    pos = nx.circular_layout(G)

    weights = nx.get_edge_attributes(G, "weight")

    nx.draw(G, pos, with_labels=True, node_size=500)
    nx.draw_networkx_edge_labels(G, pos, edge_labels=weights)

    plt.show()

def dump_graph(G, fname, mode='physical', fmode='a'):
    #with (open(fname, 'a') if fname else sys.stdout) as f:
    if fname:
        f = open(fname, fmode)
    else:
        f = sys.stdout
    
    if mode == 'physical':
        f.write(f'{len(G.nodes)}\n')
    elif mode == 'virtual':
        f.write('-\n')
    for e in G.edges.data():
        f.write(f'{e[0]} {e[1]} {int(e[2]["weight"])}\n')
    
    if fname:
        f.close()

if __name__ == "__main__":
    ph_nodes = (100, 1000)#, 10000)
    ph_links = (6, 8, 10)
    ph_p = (0, 0.5, 1)
    ph_name = ('ring', 'smwo', 'rand')
    ph_seed = 34

    rounds = 10
    round_seeds = seed_vec(rounds, None)
    vi_links = ((10, 25), (50, 100, 250))
    reps = 5

    for i, n in enumerate(ph_nodes):
        for k in ph_links:
            for p, pname in zip(ph_p, ph_name):
                for vl in vi_links[i]:
                    fname = f'topo_{n}_{k}_{pname}_v{vl}.tpl'
                    G = weight_sm_wo(n, k, p, 50, seed=ph_seed)
                    dump_graph(G, fname, fmode='w')

                    for rep in round_seeds:
                        g = weight_sm_wo(n, vl, 0.3, 1, rep)
                        dump_graph(g, fname, mode='virtual', fmode='a')

#    for n in ph_nodes:
#        for k in ph_links:
#            for p, name in zip(ph_p, ph_name):
#                for vi in vi_links:
#                    fname = f'topo_{n}_{k}_{name}_v{vi}.tpl'
#                    G = weighted_small_world(n, k, p, 50, weight_mode="uniform", seed=ph_seed)
#                    dump_graph(G, fname, fmode='w')
#                    for seed in range(n_rounds):
#                        g = weighted_small_world(n, vi, 0.1, 1, weight_mode="uniform", seed=seed)
#                        dump_graph(g, fname, mode='virtual', fmode='a')

    #n = 10
    #k = 4
    #p = 0.3
    #N = (100, 1000) #, 10000)
    #N = (10)
    #n_names = ('p')#('s', 'm', 'l')
    #k = 2
    #P = (0)#(0.25, 0.75)#(0.0, 0.5, 1.0)
    #seed = 34
    #p_names = ('0')#('025', '075')#('ring', 'sw', 'rand')

    #for n, name in zip(N, n_names):
    #    for p, pname in zip(P, p_names):
    #        G = weighted_small_world(n, k, p, weight_mode="uniform", seed=seed)
    #        dump_graph(G, f'topo_{name}_{pname}')
    #        path = nx.average_shortest_path_length(G)
    #        print(f'topo_{name}_{pname} {path}')
    
    #n = int(sys.argv[1])
    #k = int(sys.argv[2])
    #p = float(sys.argv[3])
    #if len(sys.argv == 5):
    #    w = sys.argv[4]
    #else:
    #    w = 50
#
    #G = weighted_small_world(n, k, p, w, weight_mode="uniform", seed=3)
    #dump_graph(G, None)


    #dump_graph(G, "topo1")
    #print(len(G.nodes))
    #for e in G.edges.data():
    #    print(e[0], e[1], int(e[2]["weight"]))
    #draw_graph(G)