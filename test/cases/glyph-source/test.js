export const noSnapshot = true;

export async function test(instance) {
  const results = instance.source.searchNovels("dragon", 0);
  console.log("results:", JSON.stringify(results));
  
  if (results.length !== 1) throw new Error(`Expected 1 result, got ${results.length}`);
  if (results[0].title !== "The Dragon Princess") throw new Error(`Wrong title: ${results[0].title}`);
  if (results[0].author !== "Sarah Windsworth") throw new Error(`Wrong author: ${results[0].author}`);
}
