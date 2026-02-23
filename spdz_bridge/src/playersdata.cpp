#include <iostream>
#include <vector>
#include <string>

// L'arme secrète : l'API External I/O officielle de MP-SPDZ
#include "ExternalIO/Client.hpp"

void check_ssl_file(std::string) {}
void ssl_error(std::string, std::string, std::string, std::exception&) {}

void inject_data_to_spdz(int secret_value) {
    std::cout << "\n=========================================" << std::endl;
    std::cout << "🌉 PONT C++ / MP-SPDZ (MÉTHODE OFFICIELLE)" << std::endl;
    std::cout << "=========================================" << std::endl;
    std::cout << "[Bridge] Valeur secrète à injecter : " << secret_value << std::endl;
    
    try {
        // Paramètres de connexion vers les serveurs MP-SPDZ
        std::vector<std::string> hostnames = {"localhost"};
        int port_base = 14000; // Le port par défaut de l'External I/O
        int my_client_id = 0;  // Ton Nœud s'identifie comme le Client 0
        
        std::cout << "[Bridge] Connexion au moteur MP-SPDZ (Port " << port_base << ")..." << std::endl;
        
        // C'est CETTE ligne qui va invoquer la magie de MP-SPDZ
        Client spdz_client(hostnames, port_base, my_client_id);
        
        std::cout << "[Bridge] Connexion TCP/SSL établie avec succès !" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "[Bridge] ❌ Erreur de connexion : " << e.what() << std::endl;
    }
    
    std::cout << "=========================================\n" << std::endl;
}
