#include <iostream>

// On prévient le Nœud que notre fonction d'injection existe dans le dossier d'à côté
void inject_data_to_spdz(int secret_value);

int main() {
    std::cout << "🚀 Démarrage du MPC Node (Orchestrateur Asynchrone)" << std::endl;
    std::cout << "[Node] En attente des Data Providers..." << std::endl;
    
    // Simulation : Le Nœud reçoit le salaire "4200" d'un client via le réseau
    int donnee_simulee = 4200;
    std::cout << "[Node] Donnée asynchrone reçue : " << donnee_simulee << std::endl;
    
    // On déclenche le pont pour envoyer ça à MP-SPDZ
    inject_data_to_spdz(donnee_simulee);
    
    return 0;
}
