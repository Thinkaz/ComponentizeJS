export const source = {
  searchNovels(query, page) {
    const novels = [
      { title: "Shadow Monarch", url: "/novel/shadow-monarch", author: "J.K. Nightfall" },
      { title: "Celestial Alchemy", url: "/novel/celestial-alchemy", author: "Lin Moyu" },
      { title: "Neon Requiem", url: "/novel/neon-requiem", author: "Alex Mercer" },
      { title: "The Dragon Princess", url: "/novel/dragon-princess", author: "Sarah Windsworth" },
      { title: "Infinite Regression", url: "/novel/infinite-regression", author: "Park Jinhyuk" },
    ];
    return novels.filter(n => n.title.toLowerCase().includes(query.toLowerCase()));
  }
};
